// Starling search_server — TCP server wrapper for Starling page_search.
// Protocol identical to DiskANN/PaceANN search_server so the same
// pareto_client.py can be used unchanged.
//
// Key differences from DiskANN search_server:
//   - PQFlashIndex constructor takes use_page_search + disk_file_path
//   - load_mem_index() loads the in-memory navigation graph (mem_L)
//   - Search uses page_search(query, k, mem_L, L, ...) instead of cached_beam_search
//   - et_theta from the request header is accepted but ignored (Starling has no ET)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <boost/program_options.hpp>
#include <omp.h>

#ifndef _WINDOWS
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#else
#error "search_server is currently implemented for POSIX platforms only."
#endif

#include "linux_aligned_file_reader.h"
#include "pq_flash_index.h"

namespace po = boost::program_options;

namespace
{

#pragma pack(push, 1)
struct RequestHeader
{
    uint32_t query_id;
    uint32_t k;
    uint32_t l;
    float    et_theta;  // accepted but ignored — Starling has no ET
};

struct ResponseHeader
{
    uint32_t query_id;
    uint64_t server_us;
};
#pragma pack(pop)

static_assert(sizeof(RequestHeader) == 16, "Unexpected request header size");
static_assert(sizeof(ResponseHeader) == 12, "Unexpected response header size");

bool recv_all(int fd, void *buffer, size_t length)
{
    auto *ptr = static_cast<std::uint8_t *>(buffer);
    size_t received = 0;
    while (received < length)
    {
        ssize_t rc = ::recv(fd, ptr + received, length - received, 0);
        if (rc == 0) return false;
        if (rc < 0) { if (errno == EINTR) continue; return false; }
        received += static_cast<size_t>(rc);
    }
    return true;
}

bool send_all(int fd, const void *buffer, size_t length)
{
    const auto *ptr = static_cast<const std::uint8_t *>(buffer);
    size_t sent = 0;
    while (sent < length)
    {
#ifdef MSG_NOSIGNAL
        ssize_t rc = ::send(fd, ptr + sent, length - sent, MSG_NOSIGNAL);
#else
        ssize_t rc = ::send(fd, ptr + sent, length - sent, 0);
#endif
        if (rc < 0) { if (errno == EINTR) continue; return false; }
        sent += static_cast<size_t>(rc);
    }
    return true;
}

template <typename T>
T cast_query_value(float value);
template <> float   cast_query_value<float>(float v)   { return v; }
template <> int8_t  cast_query_value<int8_t>(float v)  { return static_cast<int8_t>(std::max(-128.0f, std::min(127.0f, v))); }
template <> uint8_t cast_query_value<uint8_t>(float v) { return static_cast<uint8_t>(std::max(0.0f, std::min(255.0f, v))); }

template <typename T>
class SearchServer
{
  public:
    SearchServer(const std::string &index_prefix,
                 const std::string &disk_file_path,
                 const std::string &mem_index_path,
                 const diskann::Metric metric,
                 const uint32_t num_nodes_to_cache,
                 const uint32_t num_threads,
                 const uint32_t beamwidth,
                 const uint32_t mem_L,
                 const float    use_ratio,
                 const uint32_t query_dim)
        : _beamwidth(beamwidth), _mem_L(mem_L), _use_ratio(use_ratio)
    {
        _reader = std::shared_ptr<AlignedFileReader>(new LinuxAlignedFileReader());

        // use_page_search=true: use Starling page search (requires mem_L > 0)
        const bool use_page_search = (mem_L > 0);
        _index = std::unique_ptr<diskann::PQFlashIndex<T>>(
            new diskann::PQFlashIndex<T>(_reader, use_page_search, metric));

        const int rc = _index->load(num_threads, index_prefix.c_str(), disk_file_path);
        if (rc != 0)
            throw std::runtime_error("Unable to load index, status=" + std::to_string(rc));

        if (mem_L > 0 && !mem_index_path.empty())
        {
            _index->load_mem_index(metric, query_dim, mem_index_path, num_threads, mem_L);
            std::cout << "Loaded mem_index (mem_L=" << mem_L << ")" << std::endl;
        }

        std::vector<uint32_t> node_list;
        _index->cache_bfs_levels(num_nodes_to_cache, node_list);
        _index->load_cache_list(node_list);

        // Starling's data_dim is private; use the caller-supplied query_dim.
        // For SIFT100M uint8 this is 128.
        if (query_dim == 0)
            throw std::runtime_error("--query_dim must be specified (e.g. 128 for SIFT)");
        _dimensions        = query_dim;
        _client_dimensions = query_dim;
        omp_set_num_threads(num_threads);
    }

    void serve(const uint16_t port, const uint32_t worker_threads)
    {
        if (worker_threads == 0)
            throw std::invalid_argument("num_threads must be non-zero");

        _num_threads = worker_threads;

        int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd < 0)
            throw std::runtime_error(std::string("socket() failed: ") + std::strerror(errno));

        int opt = 1;
        ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port        = htons(port);
        if (::bind(listen_fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0)
            throw std::runtime_error(std::string("bind() failed: ") + std::strerror(errno));

        if (::listen(listen_fd, static_cast<int>(worker_threads * 16)) < 0)
            throw std::runtime_error(std::string("listen() failed: ") + std::strerror(errno));

        std::cout << "Listening on port " << port << " with " << worker_threads
                  << " worker threads" << std::endl;

        std::vector<std::thread> workers;
        workers.reserve(worker_threads);
        for (uint32_t i = 0; i < worker_threads; ++i)
            workers.emplace_back([this]() { this->worker_loop(); });

        while (true)
        {
            sockaddr_in client_addr{};
            socklen_t   client_len = sizeof(client_addr);
            const int   client_fd  = ::accept(listen_fd,
                reinterpret_cast<sockaddr *>(&client_addr), &client_len);
            if (client_fd < 0)
            {
                if (errno == EINTR) continue;
                _shutdown = true;
                _queue_cv.notify_all();
                for (auto &w : workers) if (w.joinable()) w.join();
                throw std::runtime_error(std::string("accept() failed: ") + std::strerror(errno));
            }
            {
                std::lock_guard<std::mutex> lock(_queue_mutex);
                _fd_queue.push(client_fd);
            }
            _queue_cv.notify_one();
        }
    }

  private:
    void worker_loop()
    {
        while (true)
        {
            int fd = -1;
            {
                std::unique_lock<std::mutex> lock(_queue_mutex);
                _queue_cv.wait(lock, [this]() { return !_fd_queue.empty() || _shutdown; });
                if (_shutdown && _fd_queue.empty()) return;
                fd = _fd_queue.front();
                _fd_queue.pop();
            }
            try   { handle_connection(fd); }
            catch (const std::exception &ex)
            { std::cerr << "search_server: request handling failed: " << ex.what() << std::endl; }
            ::close(fd);
        }
    }

    void handle_connection(int fd)
    {
        // Read fixed-size query header
        RequestHeader request{};
        if (!recv_all(fd, &request, sizeof(request)))
            return;

        if (request.k == 0 || request.l == 0)
        {
            std::cerr << "invalid request parameters" << std::endl;
            return;
        }

        // Read query vector (clients send float32 regardless of index type)
        std::vector<float> query_float(_client_dimensions);
        if (!recv_all(fd, query_float.data(), _client_dimensions * sizeof(float)))
            return;

        // Convert to index type
        std::vector<T> query(_dimensions, T{0});
        for (size_t i = 0; i < _client_dimensions; ++i)
            query[i] = cast_query_value<T>(query_float[i]);

        std::vector<uint64_t> result_ids(request.k);
        std::vector<float>    result_dists(request.k);

        const auto start = std::chrono::steady_clock::now();

        if (_mem_L > 0)
        {
            // Starling page search (mem_L navigation graph).
            // io_limit matches batch binary default (uint32::max = unlimited);
            // l_search alone controls beam width.
            _index->page_search(query.data(), request.k, _mem_L, request.l,
                                 result_ids.data(), result_dists.data(),
                                 _beamwidth,
                                 /*io_limit=*/std::numeric_limits<uint32_t>::max(),
                                 /*use_reorder_data=*/false,
                                 _use_ratio,
                                 /*stats=*/nullptr);
        }
        else
        {
            // Fall back to plain beam search (no mem_L)
            _index->cached_beam_search(query.data(), request.k, request.l,
                                        result_ids.data(), result_dists.data(),
                                        _beamwidth,
                                        /*use_reorder_data=*/false,
                                        /*stats=*/nullptr,
                                        /*mem_L=*/0);
        }

        const auto end = std::chrono::steady_clock::now();
        const uint64_t server_us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(end - start).count());

        ResponseHeader response_header{request.query_id, server_us};
        const size_t   response_size = sizeof(response_header)
                                     + request.k * sizeof(uint64_t)
                                     + request.k * sizeof(float);
        std::vector<uint8_t> response_buf(response_size);
        std::memcpy(response_buf.data(), &response_header, sizeof(response_header));
        std::memcpy(response_buf.data() + sizeof(response_header),
                    result_ids.data(), request.k * sizeof(uint64_t));
        std::memcpy(response_buf.data() + sizeof(response_header) + request.k * sizeof(uint64_t),
                    result_dists.data(), request.k * sizeof(float));
        send_all(fd, response_buf.data(), response_size);
    }

    std::unique_ptr<diskann::PQFlashIndex<T>> _index;
    std::shared_ptr<AlignedFileReader>         _reader;

    uint32_t    _beamwidth;
    uint32_t    _mem_L;
    float       _use_ratio;
    uint32_t    _dimensions{0};
    uint32_t    _client_dimensions{0};
    uint32_t    _num_threads{1};

    std::mutex              _queue_mutex;
    std::condition_variable _queue_cv;
    std::queue<int>         _fd_queue;
    bool                    _shutdown{false};
};

template <typename T>
int run_search_server(const std::string &index_prefix,
                      const std::string &disk_file_path,
                      const std::string &mem_index_path,
                      const diskann::Metric metric,
                      const uint32_t num_nodes_to_cache,
                      const uint32_t num_threads,
                      const uint32_t beamwidth,
                      const uint32_t mem_L,
                      const float    use_ratio,
                      const uint32_t query_dim,
                      const uint16_t port)
{
    SearchServer<T> server(index_prefix, disk_file_path, mem_index_path, metric,
                           num_nodes_to_cache, num_threads, beamwidth,
                           mem_L, use_ratio, query_dim);
    server.serve(port, num_threads);
    return 0;
}

} // anonymous namespace

int main(int argc, char **argv)
{
    std::string data_type, dist_fn, index_path_prefix, disk_file_path, mem_index_path;
    uint32_t num_threads, beamwidth, num_nodes_to_cache, mem_L, query_dim, port;
    float    use_ratio;

    po::options_description desc("Starling search_server options");
    try
    {
        desc.add_options()("help,h", "Print help");
        desc.add_options()("data_type",
            po::value<std::string>(&data_type)->required(), "float / int8 / uint8");
        desc.add_options()("dist_fn",
            po::value<std::string>(&dist_fn)->required(), "l2 / mips / cosine");
        desc.add_options()("index_path_prefix",
            po::value<std::string>(&index_path_prefix)->required(),
            "Path prefix of the DiskANN index");
        desc.add_options()("disk_file_path",
            po::value<std::string>(&disk_file_path)->default_value(""),
            "Path to the disk index file (default: index_prefix + _disk.index)");
        desc.add_options()("mem_index_path",
            po::value<std::string>(&mem_index_path)->default_value(""),
            "Path to the in-memory navigation graph (enables page_search when set)");
        desc.add_options()("query_dim",
            po::value<uint32_t>(&query_dim)->default_value(0),
            "Query dimensionality (required when mem_index_path is set)");
        desc.add_options()("port",
            po::value<uint32_t>(&port)->default_value(9002), "Listening port");
        desc.add_options()("num_threads,T",
            po::value<uint32_t>(&num_threads)->default_value(omp_get_num_procs()),
            "Number of worker threads (= async I/O contexts)");
        desc.add_options()("beam_width",
            po::value<uint32_t>(&beamwidth)->default_value(4),
            "Beamwidth for page_search / cached_beam_search");
        desc.add_options()("num_nodes_to_cache",
            po::value<uint32_t>(&num_nodes_to_cache)->default_value(0),
            "BFS nodes to cache around medoid(s)");
        desc.add_options()("mem_L",
            po::value<uint32_t>(&mem_L)->default_value(0),
            "In-memory navigation graph search list size (0 = disabled)");
        desc.add_options()("use_ratio",
            po::value<float>(&use_ratio)->default_value(1.0f),
            "Fraction of page_search candidates to use [0,1]");

        po::variables_map vm;
        po::store(po::parse_command_line(argc, argv, desc), vm);
        if (vm.count("help")) { std::cout << desc; return 0; }
        po::notify(vm);

        // Derive disk_file_path if not provided
        if (disk_file_path.empty())
            disk_file_path = index_path_prefix + "_disk.index";

        if (use_ratio < 0.0f || use_ratio > 1.0f)
        {
            std::cerr << "use_ratio must be in [0, 1]" << std::endl;
            return -1;
        }
    }
    catch (const std::exception &ex)
    {
        std::cerr << ex.what() << "\n" << desc;
        return -1;
    }

    diskann::Metric metric;
    if      (dist_fn == "l2")     metric = diskann::Metric::L2;
    else if (dist_fn == "mips")   metric = diskann::Metric::INNER_PRODUCT;
    else if (dist_fn == "cosine") metric = diskann::Metric::COSINE;
    else { std::cerr << "Unsupported distance function: " << dist_fn << std::endl; return -1; }

    try
    {
        if (data_type == "float")
            return run_search_server<float>(index_path_prefix, disk_file_path, mem_index_path,
                metric, num_nodes_to_cache, num_threads, beamwidth, mem_L, use_ratio, query_dim, port);
        else if (data_type == "int8")
            return run_search_server<int8_t>(index_path_prefix, disk_file_path, mem_index_path,
                metric, num_nodes_to_cache, num_threads, beamwidth, mem_L, use_ratio, query_dim, port);
        else if (data_type == "uint8")
            return run_search_server<uint8_t>(index_path_prefix, disk_file_path, mem_index_path,
                metric, num_nodes_to_cache, num_threads, beamwidth, mem_L, use_ratio, query_dim, port);
        else
        { std::cerr << "Unsupported data type. Use float, int8, or uint8." << std::endl; return -1; }
    }
    catch (const std::exception &e)
    {
        std::cerr << e.what() << std::endl;
        return -1;
    }
}
