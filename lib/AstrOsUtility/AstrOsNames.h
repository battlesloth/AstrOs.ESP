#ifndef ASTROSNAMES_H
#define ASTROSNAMES_H

#include <string>
#include <vector>
#include <algorithm>
#include <random>
#include "esp_random.h"

typedef std::vector<std::string> str_vec_t;

str_vec_t astrOsNames = {"Anakin", "Obi-Wan", "Ahsoka", "Ezra", "Rey", "Luke", "Mara", "Jaina", "Jacen", "Grogu"};

std::string astrOsGetName(int index)
{
    return astrOsNames[index];
}

str_vec_t astrOsRandomizeNames()
{
    str_vec_t result = {"Anakin", "Obi-Wan", "Ahsoka", "Ezra", "Rey", "Luke", "Mara", "Jaina", "Jacen", "Grogu"};

    unsigned seed = esp_random();

    // Use a mersenne_twister_engine to generate random numbers
    std::mt19937 generator(seed);

    std::shuffle(result.begin(), result.end(), generator);

    return result;
}

#endif
