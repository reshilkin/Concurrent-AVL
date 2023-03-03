//
// Created by Ravil Galiev on 30.08.2022.
//

#ifndef SETBENCH_SIMPLE_KEY_GENERATOR_H
#define SETBENCH_SIMPLE_KEY_GENERATOR_H

#include "plaf.h"
#include "common.h"

template<typename K>
class SimpleKeyGenerator : public KeyGenerator<K> {
private:
    PAD;
    Distribution *distribution;
    KeyGeneratorData<K> *data;
    PAD;

    K next() {
        K index = distribution->next();
        return data->get(index);
    }

public:
    SimpleKeyGenerator(KeyGeneratorData<K> *_data, Distribution *_distribution)
            : data(_data), distribution(_distribution) {}


    K next_read() {
        return next();
    }

    K next_erase() {
        return next();
    }

    K next_insert() {
        return next();
    }

    K next_range() {
        return next();
    }

    K next_prefill() {
        return next();
    }

    ~SimpleKeyGenerator() {
        delete distribution;
    }
};

#endif //SETBENCH_SIMPLE_KEY_GENERATOR_H
