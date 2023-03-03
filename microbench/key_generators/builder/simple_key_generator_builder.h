//
// Created by Ravil Galiev on 10.02.2023.
//

#ifndef SETBENCH_SIMPLE_KEY_GENERATOR_BUILDER_H
#define SETBENCH_SIMPLE_KEY_GENERATOR_BUILDER_H

#include "errors.h"
#include "plaf.h"
#include "common.h"

template<typename K>
struct SimpleKeyGeneratorBuilder : public KeyGeneratorBuilder<K> {
    SimpleParameters *parameters;

    SimpleKeyGeneratorBuilder(SimpleParameters *_parameters)
            : KeyGeneratorBuilder<K>(_parameters), parameters(_parameters) {
        this->keyGeneratorType = KeyGeneratorType::SIMPLE_KEYGEN;
    }

    KeyGenerator<K> **generateKeyGenerators(size_t maxkeyToGenerate, Random64 *rngs) {

        KeyGenerator<K> **keygens = new KeyGenerator<K> *[MAX_THREADS_POW2];

        if (parameters->distributionBuilder->distributionType == DistributionType::UNIFORM) {
            parameters->isNonShuffle = true;
        }

        KeyGeneratorData<K> *data = new KeyGeneratorData<K>(parameters);

#pragma omp parallel for
        for (int i = 0; i < MAX_THREADS_POW2; ++i) {
            keygens[i] = new SimpleKeyGenerator<K>(
                    data,
                    parameters->distributionBuilder->getDistribution(&rngs[i], maxkeyToGenerate)
            );
        }


        return keygens;
    }

};

#endif //SETBENCH_SIMPLE_KEY_GENERATOR_BUILDER_H
