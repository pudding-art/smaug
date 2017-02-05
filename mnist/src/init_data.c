#include "nnet_fwd.h"
#include <stdbool.h>
#include <stdio.h>

#include "utility.h"
#include "init_data.h"

void init_kernels(float* kernels, size_t k_size) {
    int i;

    for (i = 0; i < k_size; i++)
        kernels[i] = conv_float2fixed(randfloat() - 0.5);
}

void init_weights(float* weights, size_t w_size, bool random) {
    int i, ret_f_scanf;
    if (random) {
        // Randomly initialize weights
        printf("Initializing weights randomly\n");

        for (i = 0; i < w_size; i++) {
            weights[i] = conv_float2fixed((randfloat() - 0.5) *
                                          10);  // Question: does nan output
                                                // take longer in simulation?
        }
        // NOTE: FOR SIGMOID ACTIVATION FUNCTION, WEIGHTS SHOULD BE BIG
        // Otherwise everything just becomes ~0.5 after sigmoid, and results are
        // boring
    } else {
        // Read in the weights
        printf("Reading in weights from %s\n", WEIGHTS_FILENAME);

        FILE* weights_file;
        weights_file = fopen(WEIGHTS_FILENAME, "r");
        if (weights_file == NULL) {
            fprintf(stderr, "Can't open input file %s!\n", WEIGHTS_FILENAME);
            exit(1);
        }

        float read_float;
        for (i = 0; i < w_size; i++) {
            ret_f_scanf = fscanf(weights_file, "%f,", &read_float);
            weights[i] = conv_float2fixed(read_float);
        }
        fclose(weights_file);
    }
}

void init_data(float* data,
               size_t num_test_cases,
               size_t input_dim,
               bool random) {
    int i, j, ret_f_scanf;
    if (random) {
        printf("Initializing data randomly\n");
        // Generate random input data, size num_test_cases by num_units[0]
        // (input dimensionality)
        for (i = 0; i < num_test_cases * input_dim; i++) {
            data[i] = conv_float2fixed(randfloat() - 0.5);
        }
    } else {
        printf("Reading in %lu data of dimensionality %lu from %s\n",
               num_test_cases, input_dim, INPUTS_FILENAME);

        FILE* data_file;
        float read_float;
        data_file = fopen(INPUTS_FILENAME, "r");
        if (data_file == NULL) {
            fprintf(stderr, "Can't open inputs text file!\n");
            exit(1);
        }

        for (i = 0; i < num_test_cases; i++) {
            for (j = 0; j < input_dim; j++) {
                // each data point is a *ROW* !!!!!!!!!!!!!
                // this is our convention!!!!!!!!!!!!!!!!
                ret_f_scanf = fscanf(data_file, "%f,", &read_float);
                data[sub2ind(i, j, input_dim)] = conv_float2fixed(read_float);
            }
        }
        fclose(data_file);
    }

}

void init_labels(int* labels, size_t label_size, bool random) {
    int i, ret_f_scanf;
    if (random) {
        printf("Initializing labels randomly\n");
        for (i = 0; i < label_size; i++) {
            labels[i] = 0;  // set all labels to 0
        }
    } else {
        printf("Reading in %lu labels from %s\n", label_size, LABELS_FILENAME);
        FILE* labels_file;
        labels_file = fopen(LABELS_FILENAME, "r");
        if (labels_file == NULL) {
            fprintf(stderr, "Can't open labels text file.txt!\n");
            exit(1);
        }
        int read_int;
        for (i = 0; i < label_size; i++) {
            ret_f_scanf = fscanf(labels_file, "%d,", &read_int);
            labels[i] = read_int;
        }
        fclose(labels_file);
    }
}
