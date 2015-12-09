// Copyright 2014 Alessio Sclocco <a.sclocco@vu.nl>
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <iostream>
#include <string>
#include <vector>
#include <exception>
#include <fstream>
#include <iomanip>
#include <limits>
#include <algorithm>

#include <configuration.hpp>

#include <ArgumentList.hpp>
#include <Observation.hpp>
#include <InitializeOpenCL.hpp>
#include <Kernel.hpp>
#include <SNR.hpp>
#include <utils.hpp>
#include <Timer.hpp>
#include <Stats.hpp>


void initializeDeviceMemoryD(cl::Context & clContext, cl::CommandQueue * clQueue, std::vector< inputDataType > * input, cl::Buffer * input_d, cl::Buffer * output_d, const unsigned int output_size);

int main(int argc, char * argv[]) {
  bool reInit = true;
  bool DMsSamples = false;
  bool samplesDMs = false;
  unsigned int padding = 0;
	unsigned int nrIterations = 0;
	unsigned int clPlatformID = 0;
	unsigned int clDeviceID = 0;
  unsigned int vectorWidth = 0;
	unsigned int minThreads = 0;
	unsigned int maxItems = 0;
	unsigned int maxThreads = 0;
  AstroData::Observation observation;
  PulsarSearch::snrConf conf;
  cl::Event event;

	try {
    isa::utils::ArgumentList args(argc, argv);

    DMsSamples = args.getSwitch("-dms_samples");
    samplesDMs = args.getSwitch("-samples_dms");
    if ( DMsSamples && samplesDMs ) {
      std::cerr << "-dms_samples and -samples_dms are mutually exclusive." << std::endl;
      return 1;
    }
		nrIterations = args.getSwitchArgument< unsigned int >("-iterations");
		clPlatformID = args.getSwitchArgument< unsigned int >("-opencl_platform");
		clDeviceID = args.getSwitchArgument< unsigned int >("-opencl_device");
		padding = args.getSwitchArgument< unsigned int >("-padding");
    vectorWidth = args.getSwitchArgument< unsigned int >("-vector");
		minThreads = args.getSwitchArgument< unsigned int >("-min_threads");
		maxItems = args.getSwitchArgument< unsigned int >("-max_items");
		maxThreads = args.getSwitchArgument< unsigned int >("-max_threads");
    observation.setNrSamplesPerSecond(args.getSwitchArgument< unsigned int >("-samples"));
		observation.setDMRange(args.getSwitchArgument< unsigned int >("-dms"), 0.0, 0.0);
	} catch ( isa::utils::EmptyCommandLine & err ) {
		std::cerr << argv[0] << " [-dms_samples] [-samples_dms] -iterations ... -opencl_platform ... -opencl_device ... -padding ... -vector ... -min_threads ... -max_threads ... -max_items ... -dms ... -samples ..." << std::endl;
		return 1;
	} catch ( std::exception & err ) {
		std::cerr << err.what() << std::endl;
		return 1;
	}

	// Initialize OpenCL
	cl::Context clContext;
	std::vector< cl::Platform > * clPlatforms = new std::vector< cl::Platform >();
	std::vector< cl::Device > * clDevices = new std::vector< cl::Device >();
	std::vector< std::vector< cl::CommandQueue > > * clQueues = 0;

	// Allocate memory
  std::vector< inputDataType > input;
  cl::Buffer input_d, output_d;

  if ( DMsSamples ) {
    input.resize(observation.getNrDMs() * observation.getNrSamplesPerPaddedSecond(padding / sizeof(inputDataType)));
  } else if ( samplesDMs ) {
    input.resize(observation.getNrSamplesPerSecond() * observation.getNrPaddedDMs(padding / sizeof(inputDataType)));
  }

	srand(time(0));
  for ( unsigned int dm = 0; dm < observation.getNrDMs(); dm++ ) {
    for ( unsigned int sample = 0; sample < observation.getNrSamplesPerSecond(); sample++ ) {
      if ( DMsSamples ) {
        input[(dm * observation.getNrSamplesPerPaddedSecond(padding / sizeof(inputDataType))) + sample] = static_cast< inputDataType >(rand() % 10);
      } else if ( samplesDMs ) {
        input[(sample * observation.getNrPaddedDMs(padding / sizeof(inputDataType))) + dm] = static_cast< inputDataType >(rand() % 10);
      }
    }
  }

	std::cout << std::fixed << std::endl;
  std::cout << "# nrDMs nrSamples threadsD0 itemsD0 GB/s time stdDeviation COV" << std::endl << std::endl;

	for ( unsigned int threads = minThreads; threads <= maxThreads; threads++ ) {
    if ( threads % vectorWidth != 0 ) {
      continue;
    }
    conf.setNrThreadsD0(threads);

    for ( unsigned int itemsPerThread = 1; (itemsPerThread * 4) < maxItems; itemsPerThread++ ) {
      if ( DMsSamples ) {
        if ( observation.getNrSamplesPerSecond() % (itemsPerThread * conf.getNrThreadsD0()) != 0 ) {
          continue;
        }
      } else if ( samplesDMs ) {
        if ( observation.getNrDMs() % ( itemsPerThread * conf.getNrThreadsD0()) != 0 ) {
          continue;
        }
      }
      conf.setNrItemsD0(itemsPerThread);

      // Generate kernel
      double gbs = isa::utils::giga((static_cast< uint64_t >(observation.getNrDMs()) * observation.getNrSamplesPerSecond() * sizeof(inputDataType)) + (static_cast< uint64_t >(observation.getNrDMs()) * sizeof(float)));
      cl::Kernel * kernel;
      isa::utils::Timer timer;
      std::string * code;
      if ( DMsSamples ) {
        code = PulsarSearch::getSNRDMsSamplesOpenCL< inputDataType >(conf, inputDataName, observation.getNrSamplesPerSecond(), padding);
      } else if ( samplesDMs ) {
        code = PulsarSearch::getSNRSamplesDMsOpenCL< inputDataType >(conf, inputDataName, observation, padding);
      }

      if ( reInit ) {
        delete clQueues;
        clQueues = new std::vector< std::vector< cl::CommandQueue > >();
        isa::OpenCL::initializeOpenCL(clPlatformID, 1, clPlatforms, &clContext, clDevices, clQueues);
        try {
          initializeDeviceMemoryD(clContext, &(clQueues->at(clDeviceID)[0]), &input, &input_d, &output_d, observation.getNrDMs() * sizeof(float));
        } catch ( cl::Error & err ) {
          return -1;
        }
        reInit = false;
      }
      try {
        if ( DMsSamples ) {
          kernel = isa::OpenCL::compile("snrDMsSamples" + isa::utils::toString(observation.getNrSamplesPerSecond()), *code, "-cl-mad-enable -Werror", clContext, clDevices->at(clDeviceID));
        } else if ( samplesDMs ) {
          kernel = isa::OpenCL::compile("snrSamplesDMs" + isa::utils::toString(observation.getNrDMs()), *code, "-cl-mad-enable -Werror", clContext, clDevices->at(clDeviceID));
        }
      } catch ( isa::OpenCL::OpenCLError & err ) {
        std::cerr << err.what() << std::endl;
        delete code;
        break;
      }
      delete code;

      cl::NDRange global, local;
      if ( DMsSamples ) {
        global = cl::NDRange(conf.getNrThreadsD0(), observation.getNrDMs());
        local = cl::NDRange(conf.getNrThreadsD0(), 1);
      } else if ( samplesDMs ) {
        global = cl::NDRange(observation.getNrDMs() / conf.getNrItemsD0());
        local = cl::NDRange(conf.getNrThreadsD0());
      }

      kernel->setArg(0, input_d);
      kernel->setArg(1, output_d);

      try {
        // Warm-up run
        clQueues->at(clDeviceID)[0].finish();
        clQueues->at(clDeviceID)[0].enqueueNDRangeKernel(*kernel, cl::NullRange, global, local, 0, &event);
        event.wait();
        // Tuning runs
        for ( unsigned int iteration = 0; iteration < nrIterations; iteration++ ) {
          timer.start();
          clQueues->at(clDeviceID)[0].enqueueNDRangeKernel(*kernel, cl::NullRange, global, local, 0, &event);
          event.wait();
          timer.stop();
        }
      } catch ( cl::Error & err ) {
        std::cerr << "OpenCL error kernel execution (";
        std::cerr << conf.print();
        std::cerr << "): " << isa::utils::toString(err.err()) << "." << std::endl;
        delete kernel;
        if ( err.err() == -4 || err.err() == -61 ) {
          return -1;
        }
        reInit = true;
        break;
      }
      delete kernel;

      std::cout << observation.getNrDMs() << " " << observation.getNrSamplesPerSecond() << " ";
      std::cout << conf.print() << " ";
      std::cout << std::setprecision(3);
      std::cout << gbs / timer.getAverageTime() << " ";
      std::cout << std::setprecision(6);
      std::cout << timer.getAverageTime() << " " << timer.getStandardDeviation() << " " << timer.getCoefficientOfVariation() << std::endl;
    }
  }

	std::cout << std::endl;

	return 0;
}

void initializeDeviceMemoryD(cl::Context & clContext, cl::CommandQueue * clQueue, std::vector< inputDataType > * input, cl::Buffer * input_d, cl::Buffer * output_d, const unsigned int output_size) {
  try {
    *input_d = cl::Buffer(clContext, CL_MEM_READ_WRITE, input->size() * sizeof(inputDataType), 0, 0);
    *output_d = cl::Buffer(clContext, CL_MEM_WRITE_ONLY, output_size, 0, 0);
    clQueue->enqueueWriteBuffer(*input_d, CL_FALSE, 0, input->size() * sizeof(inputDataType), reinterpret_cast< void * >(input->data()));
    clQueue->finish();
  } catch ( cl::Error & err ) {
    std::cerr << "OpenCL error: " << isa::utils::toString< cl_int >(err.err()) << "." << std::endl;
    throw;
  }
}

