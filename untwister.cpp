/*
    Copyright Dan Petro, 2014

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <limits.h>
#include <stdint.h>
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <thread>

#include "ConsoleColors.h"
#include "PRNGFactory.h"
#include "prngs/PRNG.h"

using std::chrono::seconds;
using std::chrono::milliseconds;
using std::chrono::duration_cast;
using std::chrono::steady_clock;

// Pair of <seed, quality of fit>
typedef std::pair<uint32_t, double> Seed;

static std::vector<uint32_t> observedOutputs;
static const unsigned int ONE_YEAR = 31536000;

void Usage(PRNGFactory factory, unsigned int threads)
{
    std::cout << BOLD << "Untwister" << RESET << " - Recover PRNG seeds from observed values." << std::endl;
    std::cout << "\t-i <input_file> [-d <depth> ] [-r <prng>] [-g <seed>] [-t <threads>] [-c <confidence>]\n" << std::endl;
    std::cout << "\t-i <input_file>\n\t\tPath to file input file containing observed results of your RNG. The contents" << std::endl;
    std::cout << "\t\tare expected to be newline separated 32-bit integers. See test_input.txt for" << std::endl;
    std::cout << "\t\tan example." << std::endl;
    std::cout << "\t-d <depth>\n\t\tThe depth (default 1000) to inspect for each seed value when brute forcing." << std::endl;
    std::cout << "\t\tChoosing a higher depth value will make brute forcing take longer (linearly), but is" << std::endl;
    std::cout << "\t\trequired for cases where the generator has been used many times already." << std::endl;
    std::cout << "\t-r <prng>\n\t\tThe PRNG algorithm to use. Supported PRNG algorithms:" << std::endl;
    std::vector<std::string> names = factory.getNames();
    for (unsigned int index = 0; index < names.size(); ++index)
    {
        std::cout << "\t\t" << BOLD << " * " << RESET << names[index];
        if (index == 0)
            std::cout << " (default)";
        std::cout << std::endl;
    }
    std::cout << "\t-u\n\t\tUse bruteforce, but only for unix timestamp values within a range of +/- 1 " << std::endl;
    std::cout << "\t\tyear from the current time." << std::endl;
    std::cout << "\t-g <seed>\n\t\tGenerate a test set of random numbers from the given seed (at a random depth)" << std::endl;
    std::cout << "\t-c <confidence>\n\t\tSet the minimum confidence percentage to report" << std::endl;
    std::cout << "\t-t <threads>\n\t\tSpawn this many threads (default is " << threads << ")" << std::endl;
    std::cout << "" << std::endl;
}


/* Yeah lots of parameters, but such is the life of a thread */
void BruteForce(const unsigned int id, bool& isCompleted, std::vector<std::vector<Seed>* > *answers,
        std::vector<uint32_t>* status, double minimumConfidence, uint32_t startingSeed, uint32_t endingSeed,
        uint32_t depth, std::string rng)
{
    /* Each thread must have a local factory unless you like mutexes and/or segfaults */
    PRNGFactory factory;
    PRNG *generator = factory.getInstance(rng);
    answers->at(id) = new std::vector<Seed>;

    for (uint32_t seedIndex = startingSeed; seedIndex <= endingSeed; ++seedIndex)
    {
        generator->seed(seedIndex);

        uint32_t matchesFound = 0;
        for (uint32_t index = 0; index < depth; index++)
        {
            uint32_t nextRand = generator->random();
            uint32_t observed = observedOutputs[matchesFound];

            if (observed == nextRand)
            {
                matchesFound++;
                if (matchesFound == observedOutputs.size())
                {
                    break;  // This seed is a winner if we get to the end
                }
            }
        }

        if (isCompleted)
        {
            break;  // Some other thread found the seed
        }

        status->at(id) = seedIndex - startingSeed;
        double confidence = ((double) matchesFound / (double) observedOutputs.size()) * 100.0;
        if (minimumConfidence <= confidence)
        {
            Seed seed = {seedIndex, confidence};
            answers->at(id)->push_back(seed);
        }
        if (matchesFound == observedOutputs.size())
            isCompleted = true;  // We found the correct seed
    }
    delete generator;
}

/* For easier testing, will generate a series of random numbers at a given seed */
void GenerateSample(uint32_t seed, uint32_t depth, std::string rng)
{
    PRNGFactory factory;
    PRNG *generator = factory.getInstance(rng);
    generator->seed(seed);

    for (unsigned int index = 0; index < depth; ++index)
    {
        std::cout << generator->random() << std::endl;
    }
    delete generator;
}

void StatusThread(std::vector<std::thread>& pool, bool& isCompleted, uint32_t totalWork, std::vector<uint32_t> *status)
{
    double percent = 0;
    steady_clock::time_point start = steady_clock::now();
    while (!isCompleted)
    {
        unsigned int sum = 0;
        for (unsigned int index = 0; index < status->size(); ++index)
        {
            sum += status->at(index);
        }
        percent = ((double) sum / (double) totalWork) * 100.0;
        isCompleted = (100.0 <= percent);
        std::cout << "\rProgress: " << CLEAR.c_str() << DEBUG.c_str() << percent << "%";
        std::cout << " (" << (int) duration_cast<seconds>(steady_clock::now() - start).count() << "seconds)";
        std::cout.flush();
        std::this_thread::sleep_for(milliseconds(150));
    }
    std::cout << "\r" << CLEAR.c_str();
}

/* Divide X number of seeds among Y number of threads */
std::vector<uint32_t> DivisionOfLabor(uint32_t sizeOfWork, uint32_t numberOfWorkers)
{
    uint32_t work = sizeOfWork / numberOfWorkers;
    uint32_t leftover = sizeOfWork % numberOfWorkers;
    std::vector<uint32_t> labor(numberOfWorkers);
    for (uint32_t index = 0; index < numberOfWorkers; ++index)
    {
        if (0 < leftover)
        {
            labor[index] = work + 1;
            --leftover;
        }
        else
        {
            labor[index] = work;
        }
    }
    return labor;
}

void SpawnThreads(const unsigned int threads, std::vector<std::vector<Seed>* > *answers, double minimumConfidence,
        uint32_t lowerBoundSeed, uint32_t upperBoundSeed, uint32_t depth, std::string rng)
{
    bool isCompleted = false;  // Flag to tell threads to stop working
    std::cout << INFO << "Spawning " << threads << " worker thread(s) ..." << std::endl;

    std::vector<std::thread> pool(threads);
    std::vector<uint32_t> *status = new std::vector<uint32_t>(threads);
    std::vector<uint32_t> labor = DivisionOfLabor(upperBoundSeed - lowerBoundSeed, threads);
    uint32_t startAt = lowerBoundSeed;
    for (unsigned int id = 0; id < threads; ++id)
    {
        uint32_t endAt = startAt + labor.at(id);
        pool[id] = std::thread(BruteForce, id, std::ref(isCompleted), answers, status, minimumConfidence, startAt, endAt, depth, rng);
        startAt += labor.at(id);
    }
    StatusThread(pool, isCompleted, upperBoundSeed - lowerBoundSeed, status);
    for (unsigned int id = 0; id < pool.size(); ++id)
    {
        pool[id].join();
    }

    delete status;
}

void FindSeed(const std::string& rng, unsigned int threads, double miniumConfidence, uint32_t lowerBoundSeed,
        uint32_t upperBoundSeed, uint32_t depth)
{
    std::cout << INFO << "Brute Forcing for seed using " << rng << std::endl;

    /* Each thread needs their own set of answers to avoid locking */
    std::vector<std::vector<Seed>* > *answers = new std::vector<std::vector<Seed>* >(threads);
    steady_clock::time_point elapsed = steady_clock::now();
    SpawnThreads(threads, answers, miniumConfidence, lowerBoundSeed, upperBoundSeed, depth, rng);

    std::cout << INFO << "Completed in " << duration_cast<seconds>(steady_clock::now() - elapsed).count()
              << " second(s)" << std::endl;

    /* Display results */
    for (unsigned int id = 0; id < answers->size(); ++id)
    {
        /* Look for answers from each thread */
        for (unsigned int index = 0; index < answers->at(id)->size(); ++index)
        {
            std::cout << SUCCESS << "Found seed " << answers->at(id)->at(index).first
                      << " with a confidence of " << answers->at(id)->at(index).second
                      << '%' << std::endl;
        }
        delete answers->at(id);
    }
    delete answers;
}

/* 
    This is the "smarter" method of breaking RNGs. We use consecutive integers
    to infer information about the internal state of the RNG. Using this 
    method, however, we won't typically recover an actual seed value. 
    But the effect is the same.
*/
bool InferState(const std::string& rng)
{
    std::cout << INFO << "Trying state inference" << std::endl;

    PRNGFactory factory;
    PRNG *generator = factory.getInstance(rng);
    uint32_t stateSize = generator->getStateSize();

    if(observedOutputs.size() <= stateSize)
    {
        std::cout << WARN << "Not enough observed values to perform state inference." << std::endl;
        std::cout << WARN << "Try again with more than " << stateSize << " values" << std::endl;
        return false;
    }

    double highscore = 0.0;

    /* Guaranteed from the above to loop at least one time */
    std::vector<double> scores;
    std::vector<uint32_t> best_state;
    for(uint32_t i = 0; i < (observedOutputs.size() - stateSize); i++)
    {
        std::vector<uint32_t>::const_iterator first = observedOutputs.begin() + i;
        std::vector<uint32_t>::const_iterator last = observedOutputs.begin() + i + stateSize;
        std::vector<uint32_t> state(first, last);

        /* Make predictions based on the state */
        std::vector<uint32_t> evidenceForward
            ((std::vector<uint32_t>::const_iterator)observedOutputs.begin(), first);
        std::vector<uint32_t> evidenceBackward
            (last+1, (std::vector<uint32_t>::const_iterator)observedOutputs.end());
        generator->setState(state);

        /* Provide additional evidence for tuning on PRNGs that require it */
        generator->setEvidence(observedOutputs);
        generator->tune(evidenceForward, evidenceBackward);

        std::vector<uint32_t> predictions_forward = 
            generator->predictForward(((observedOutputs.size() - stateSize) - i));
        std::vector<uint32_t> predictions_backward = 
            generator->predictBackward(i);

        /* Test the prediction against the rest of the observed data */
        /* Forward */
        uint32_t matchesFound = 0;
        uint32_t index_pred = 0;
        uint32_t index_obs = i + stateSize;
        while(index_obs < observedOutputs.size() && index_pred < predictions_forward.size())
        {
            if(observedOutputs[index_obs] == predictions_forward[index_pred])
            {
                matchesFound++;
                index_obs++;
            }
            index_pred++;
        }

        /* Backward */
        index_pred = 0;
        index_obs = i;
        while(index_obs > 0 && index_pred < predictions_backward.size())
        {
            if(observedOutputs[index_obs] == predictions_backward[index_pred])
            {
                matchesFound++;
                index_obs--;
            }
            index_pred++;
        }

        /* If we get a perfect guess, then try reversing out the seed, and exit */
        if(matchesFound == (observedOutputs.size() - stateSize))
        {
            uint32_t outSeed = 0;
            if(generator->reverseToSeed(&outSeed, 10000))
            {
                /* We win! */
                std::cout << SUCCESS << "Found seed " << outSeed << std::endl;
            }
            else
            {
                std::cout << SUCCESS << "Found state: " << std::endl;
                std::vector<uint32_t> state = generator->getState();
                for(uint32_t j = 0; j < state.size(); j++)
                {
                    std::cout << SUCCESS << state[j] << std::endl;
                }
            }
            return true;
        }

        double score = (double)(matchesFound*100) / (double)(observedOutputs.size() - stateSize);
        scores.push_back(score);
        if(score > highscore)
        {
            best_state = generator->getState();
        }
    }

    /* Analyze scores */
    //TODO
    if(highscore > 0)
    {
        std::cout << SUCCESS << "Best state guess, with confidence of: " << highscore << "%" << std::endl;
        std::vector<uint32_t> state = generator->getState();
        for(uint32_t j = 0; j < state.size(); j++)
        {
            std::cout << SUCCESS << state[j] << std::endl;
        }
    }
    else
    {
        std::cout << INFO << "State Inference failed" << std::endl;
    }

    return false;
}

int main(int argc, char *argv[])
{
    int c;
    unsigned int threads = std::thread::hardware_concurrency();
    uint32_t lowerBoundSeed = 0;
    uint32_t upperBoundSeed = UINT_MAX;
    uint32_t depth = 1000;
    uint32_t seed = 0;
    double minimumConfidence = 100.0;
    PRNGFactory factory;
    std::string rng = factory.getNames()[0];

    while ((c = getopt(argc, argv, "d:i:g:t:r:c:uh")) != -1)
    {
        switch (c)
        {
            case 'g':
            {
                seed = strtoul(optarg, NULL, 10);
                break;
            }
            case 'u':
            {
                lowerBoundSeed = time(NULL) - ONE_YEAR;
                upperBoundSeed = time(NULL) + ONE_YEAR;
                break;
            }
            case 'r':
            {
                rng = optarg;
                std::vector<std::string> names = factory.getNames();
                if (std::find(names.begin(), names.end(), rng) == names.end())
                {
                    std::cerr << WARN << "ERROR: The PRNG \"" << optarg << "\" is not supported, see -h" << std::endl;
                    return EXIT_FAILURE;
                }
                break;
            }
            case 'd':
            {
                depth = strtoul(optarg, NULL, 10);
                if (depth == 0)
                {
                    std::cerr << WARN << "ERROR: Please enter a valid depth > 1" << std::endl;
                    return EXIT_FAILURE;
                }
                break;
            }
            case 'i':
            {
                std::ifstream infile(optarg);
                if (!infile)
                {
                    std::cerr << WARN << "ERROR: File \"" << optarg << "\" not found" << std::endl;
                }
                std::string line;
                while (std::getline(infile, line))
                {
                    observedOutputs.push_back(strtoul(line.c_str(), NULL, 10));
                }
                break;
            }
            case 't':
            {
                threads = strtoul(optarg, NULL, 10);
                if (threads == 0)
                {
                    std::cerr << WARN << "ERROR: Please enter a valid number of threads > 1" << std::endl;
                    return EXIT_FAILURE;
                }
                break;
            }
            case 'c':
            {
                minimumConfidence = ::atof(optarg);
                if (minimumConfidence <= 0 || 100.0 < minimumConfidence)
                {
                    std::cerr << WARN << "ERROR: Invalid confidence percentage " << std::endl;
                    return EXIT_FAILURE;
                }
                break;
            }
            case 'h':
            {
                Usage(factory, threads);
                return EXIT_SUCCESS;
            }
            case '?':
            {
                if (optopt == 'd')
                   std::cerr << "Option -" << optopt << " requires an argument." << std::endl;
                else if (isprint(optopt))
                   std::cerr << "Unknown option `-" << optopt << "'." << std::endl;
                else
                   std::cerr << "Unknown option character `" << optopt << "'." << std::endl;
                Usage(factory, threads);
                return EXIT_FAILURE;
            }
            default:
            {
                Usage(factory, threads);
                return EXIT_FAILURE;
            }
        }
    }

    if (seed != 0)
    {
        GenerateSample(seed, depth, rng);
        return EXIT_SUCCESS;
    }

    if (observedOutputs.empty())
    {
        Usage(factory, threads);
        std::cerr << WARN << "ERROR: No input numbers provided. Use -i <file> to provide a file" << std::endl;
        return EXIT_FAILURE;
    }

    if(InferState(rng))
    {
        return EXIT_SUCCESS;
    }

    FindSeed(rng, threads, minimumConfidence, lowerBoundSeed, upperBoundSeed, depth);
    return EXIT_SUCCESS;
}

