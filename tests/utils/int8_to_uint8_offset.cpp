// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT license.

#include <cstdint>
#include <iostream>

#include "utils.h"

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cout << argv[0] << " input_int8_bin output_uint8_bin" << std::endl;
    return -1;
  }

  int8_t* input = nullptr;
  size_t npts = 0, nd = 0;
  diskann::load_bin<int8_t>(argv[1], input, npts, nd);

  auto* output = new uint8_t[npts * nd];
  for (size_t i = 0; i < npts * nd; ++i) {
    output[i] = static_cast<uint8_t>(static_cast<int16_t>(input[i]) + 128);
  }

  diskann::save_bin<uint8_t>(argv[2], output, npts, nd);
  delete[] output;
  delete[] input;
  return 0;
}
