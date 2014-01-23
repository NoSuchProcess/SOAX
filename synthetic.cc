/*
 * File: synthetic.cc
 *
 * This file implements the generation of synthetic curvilinear
 * network images with uniform foreground and various level of
 * Gaussian noise. The input is a snake file (in JFilament format) and
 * the output are a bunch of images in the specified folder.
 *
 * Copyright (C) Ting Xu 2014, IDEA Lab, Lehigh University
 */

#include <iostream>
#include <sstream>
#include "multisnake.h"

int main(int argc, char **argv) {
  if (argc < 4) {
    std::cout << "Usage: ./syn <input-image> <input-snake> <output-directory>"
              << std::endl;
    return -1;
  }

  soax::Multisnake multisnake;
  multisnake.LoadImage(argv[1]);
  multisnake.LoadGroundTruthSnakes(argv[2]);
  std::cout << multisnake.GetNumberOfComparingSnakes1()
            << " ground truth snakes loaded." << std::endl;

  const unsigned background = 200;
  for (int i = 1; i < 16; i++) {
    double sigma = i;
    for (unsigned i = 1; i < 5; i++) {
      unsigned foreground = 100 * i;
      std::ostringstream buffer;
      buffer << "fg" << foreground << "-sigma" << sigma << ".mha";
      multisnake.GenerateSyntheticImage(
          foreground, background, sigma,
          std::string(argv[3]) + buffer.str());
    }
  }
  return 0;
}