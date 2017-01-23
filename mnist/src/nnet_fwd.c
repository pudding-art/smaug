#include <assert.h>
#include <stdbool.h>
#include <stdio.h>

#include "activation_functions.h"
#include "init_data.h"
#include "utility.h"

#ifdef DMA_MODE
#include "gem5_harness.h"
#endif

#ifdef GEM5_HARNESS
#include "gem5/aladdin_sys_connection.h"
#include "gem5/aladdin_sys_constants.h"
#endif

#include "nnet_fwd.h"

// All the memory used in nnet:
// name           | type  | size/value
// ---------------|-------|--------------
// data           | float | NUM_TEST_CASES*INPUT_DIM
// weights        | float | INPUT_DIM * NUM_UNITS_1 +
//                |       | NUM_UNITS_1 * NUM_UNITS_2 +
//                |       | NUM_UNITS_2 * NUM_CLASSES
// num_test_cases | int   | NUM_TEST_CASES
// num_layers     | int   | NUM_LAYERS
// num_units      | int   | NUM_LAYERS + 2
// activation_fun | int   | ACTIVATION_FUN
// num_rows       | int   | NUM_LAYERS + 1
// num_colums     | int   | NUM_LAYERS + 1
// hid            | float | NUM_TEST_CASES * BIGGEST_ROW
// hid_temp       | float | NUM_TEST_CASES * BIGGEST_ROW

// int NUM_HIDDEN_UNITS[NUM_LAYERS] = { 79,74,71 };
int NUM_HIDDEN_UNITS[NUM_LAYERS] = { 5 };

// Grab matrix n out of the doubly flattened w
// (w is a flattened collection of matrices, each flattened)
float* grab_matrix(float* w, int n, int* n_rows, int* n_columns) {
    int ind = 0;
    int i;
grab_matrix_loop:    for (i = 0; i < n; i++) {
        ind += n_rows[i] * n_columns[i];
    }
    return w + ind;
}

#ifdef DMA_MODE
void grab_matrix_dma(float* weights,
                     int layer,
                     int* n_rows,
                     int* n_columns) {
    size_t offset = 0;
    int i;
grab_matrix_dma_loop:    for (i = 0; i < layer; i++) {
        offset += n_rows[i] * n_columns[i];
    }
    size_t size = n_rows[layer] * n_columns[layer] * sizeof(float);
    dmaLoad(weights, offset*sizeof(float), 0, size);
}
#endif

// Multiply matrices a and b with given sizes and store into result_goes_here.
//
// We could do something tricky by switching the role of result and temp, to
// avoid copying but let's leave that for now.
//
// result_temp is used to ensure that weird things don't happen if
// result_goes_here overlaps with a or b.
void matrix_multiply(float* a,
                     float* b,
                     int a_height,
                     int a_width_b_height,
                     int b_width,
                     float* result_goes_here,
                     float* result_temp) {

    int i, j, k;
    float value;

    // Initialize to zero
    int size = a_height * b_width;
    clear_matrix(result_temp, size);

matmul0:    for (i = 0; i < a_height; i++) {
matmul1:        for (j = 0; j < b_width; j++) {
matmul2:            for (k = 0; k < a_width_b_height; k++) {
                value = conv_float2fixed(a[sub2ind(i, k, a_width_b_height)]) *
                        conv_float2fixed(b[sub2ind(k, j, b_width)]);
                result_temp[sub2ind(i, j, b_width)] =
                        conv_float2fixed(result_temp[sub2ind(i, j, b_width)] +
                                         conv_float2fixed(value));
            }
        }
    }
    copy_matrix(result_temp, result_goes_here, size);
}

// Multiply matrices a and b, assuming the last row of b are biases.
//
// So we expect a_width = b_height - 1.
void matrix_multiply_with_bias(float* a,
                               float* b,
                               int a_height,
                               int b_height,
                               int b_width,
                               float* result) {

    // a is hid, b is weights
    int i, j, k;
    float partial_sum;
    float value;

matmulb0: for (i = 0; i < a_height; i++) {
matmulb1: for (j = 0; j < b_width; j++) {
            // Initialize to zero
            partial_sum = 0;
matmulb2: for (k = 0; k < b_height; k++) {
                value = conv_float2fixed(a[sub2ind(i, k, b_height)]) *
                        conv_float2fixed(b[sub2ind(k, j, b_width)]);
                partial_sum += value;
                /*
                result[sub2ind(i, j, b_width)] =
                        conv_float2fixed(result[sub2ind(i, j, b_width)] +
                                         conv_float2fixed(value));
                                         */
            }
            // Add the bias.
            partial_sum += conv_float2fixed(b[sub2ind(b_height, j, b_width)]);
            result[sub2ind(i, j, b_width)] = partial_sum;
            /*
                    conv_float2fixed(result[sub2ind(i, j, b_width)] +
                                     b[sub2ind(b_height, j, b_width)]);
                                     */
        }
    }
}

void matrix_multiply_with_bias_and_copy(float* a,
                                        float* b,
                                        int a_height,
                                        int b_height,
                                        int b_width,
                                        float* result_goes_here,
                                        float* result_temp) {
    int size = a_height * b_width;
    matrix_multiply_with_bias(
            a, b, a_height, b_height, b_width, result_temp);
    copy_matrix(result_temp, result_goes_here, size);
}

// Multiply the matrices a and b, but assume that b has been transposed.
void matrix_multiply_with_bias_transpose(float* a,
                                         float* b,
                                         int a_height,
                                         int b_height,
                                         int b_width,
                                         float* result) {

    // a is hid, b is weights
    int i, j, k;
    float partial_sum;
    float value;

matmulbt0: for (i = 0; i < a_height; i++) {
matmulbt1: for (j = 0; j < b_width; j++) {
            // Initialize to zero
            partial_sum = 0;
matmulbt2: for (k = 0; k < b_height; k++) {
                value = conv_float2fixed(a[sub2ind(i, k, b_width)]) *
                        conv_float2fixed(b[sub2ind(j, k, b_height)]);
                partial_sum += value;
            }
            // Add the bias.
            partial_sum += conv_float2fixed(b[sub2ind(j, b_height, b_height)]);
            result[sub2ind(i, j, b_width)] = partial_sum;
        }
    }
}

// Dispatch to the appropriate activation function.
void activation_fun(float* hid, int size, float* sigmoid_table) {
    if (ACTIVATION_FUN == 0) {
        RELU(hid, size);
    } else if (ACTIVATION_FUN == 1) {
        sigmoid_lookup(hid, size, sigmoid_table);
    } else {
        sigmoidn(hid, size);
    }
}

void print_debug(float* hid,
                 int rows_to_print,
                 int cols_to_print,
                 int num_columns) {
    int i, l;
    printf("\nHidden units:\n");
    for (i = 0; i < rows_to_print; i++) {
        for (l = 0; l < cols_to_print; l++) {
            printf("%f, ", hid[sub2ind(i, l, num_columns)]);
        }
        printf("\n");
    }
}

// Does the forward predictive pass of a neural net.
// Returns a float array of class predictions in row major format of size
// num_test_cases*num_labels
void nnet_fwd(float* data,
              float* weights,
              int* num_units,
              int* num_rows,
              int* num_columns,
              float* hid,
              float* hid_temp,
              float* sigmoid_table) {

    int i, l;

    if (DEBUG == 1) {
        printf("\nDATA:\n");
        for (i = 0; i < NUM_TEST_CASES; i++) {
            printf("Datum %d:\n", i);
            for (l = 0; l < INPUT_DIM; l++) {
                printf("%e, ", data[sub2ind(i, l, NUM_TEST_CASES)]);
            }
            printf("\n");
        }

        printf("\nWEIGHTS:\n\n");
        for (l = 0; l < num_rows[0] * num_columns[0]; l++) {
            printf("%f\n", weights[l]);
        }
        printf("\nEND WEIGHTS:\n\n");
    }

    // FORMAT HERE IS H TIMES W, NOT W TIMES H!!!!!
    // SO EACH DATA POINT IS A ***ROW****

#ifdef DMA_MODE
    dmaLoad(data, 0, 0, NUM_TEST_CASES*INPUT_DIM*sizeof(float));
    grab_matrix_dma(weights, 0, num_rows, num_columns);
#else
    // Don't need to grab 0th matrix.
#endif

    // FIRST LAYER. hid should be num_test_cases x num_units[1]
    matrix_multiply_with_bias_transpose(
            data, weights, NUM_TEST_CASES, num_units[0], num_units[1], hid);

    // Rows to print, cols to print, number of cols
    PRINT_DEBUG(hid, NUM_TEST_CASES, num_units[1], num_units[1]);

    // Pass through activation function
    activation_fun(hid, NUM_TEST_CASES * num_units[1], sigmoid_table);

    PRINT_DEBUG(hid, NUM_TEST_CASES, num_units[1], num_units[1]);

nnet_fwd_layer_loop:    for (l = 1; l < NUM_LAYERS; l++) {
        // Get hidden activations
#ifdef DMA_MODE
        grab_matrix_dma(weights, l, num_rows, num_columns);
#endif
        // Alternate between reading from hid and hid_temp so we can avoid
        // copying matrices. Odd layers must read from hid since that's where
        // the first layer puts the output.
        if (l % 2 == 0) {
            matrix_multiply_with_bias_transpose(hid_temp, weights,
                                                NUM_TEST_CASES, num_units[l],
                                                num_units[l + 1], hid);
            PRINT_DEBUG(hid, NUM_TEST_CASES, num_units[l + 1], num_units[l + 1]);
            // Pass through activation function
            activation_fun(hid, NUM_TEST_CASES * num_units[l + 1], sigmoid_table);
            PRINT_DEBUG(hid, NUM_TEST_CASES, num_units[l + 1], num_units[l + 1]);
        } else {
            matrix_multiply_with_bias_transpose(hid, weights, NUM_TEST_CASES,
                                                num_units[l], num_units[l + 1],
                                                hid_temp);
            PRINT_DEBUG(hid_temp, NUM_TEST_CASES, num_units[l + 1], num_units[l + 1]);
            // Pass through activation function
            activation_fun(hid_temp, NUM_TEST_CASES * num_units[l + 1], sigmoid_table);
            PRINT_DEBUG(hid_temp, NUM_TEST_CASES, num_units[l + 1], num_units[l + 1]);
        }
    }

#ifdef DMA_MODE
    grab_matrix_dma(weights, NUM_LAYERS, num_rows, num_columns);
#endif
    if (NUM_LAYERS % 2 == 0) {
        matrix_multiply_with_bias_transpose(hid_temp, weights, NUM_TEST_CASES,
                                  num_units[NUM_LAYERS],
                                  num_units[NUM_LAYERS + 1], hid);
        PRINT_DEBUG(hid, NUM_TEST_CASES, NUM_CLASSES, NUM_CLASSES);
    } else {
        matrix_multiply_with_bias_transpose(hid, weights, NUM_TEST_CASES,
                                  num_units[NUM_LAYERS],
                                  num_units[NUM_LAYERS + 1], hid_temp);
        PRINT_DEBUG(hid_temp, NUM_TEST_CASES, NUM_CLASSES, NUM_CLASSES);
    }
    // hid now contains the output


    // we now apply the softmax to turn the outputs into class probabilities
    // softmax(hid, NUM_TEST_CASES, num_units[NUM_LAYERS+1]);
    // PRINT_DEBUG(hid, 10, NUM_CLASSES, NUM_CLASSES);
#ifdef DMA_MODE
    if (NUM_LAYERS % 2 == 0)
        dmaStore(hid, 0, 0, NUM_TEST_CASES * NUM_CLASSES * sizeof(float));
    else
        dmaStore(hid_temp, 0, 0, NUM_TEST_CASES * NUM_CLASSES * sizeof(float));
#endif
}

// This is the thing that we want to be good at in hardware
int main(int argc, const char* argv[]) {
    // set random seed (need to #include <time.h>)
    srand(1);

    int i;
    int num_units[NUM_LAYERS + 2];

    num_units[0] = INPUT_DIM;  // input dimensionality
    for (i = 1; i <= NUM_LAYERS; i++) {
        num_units[i] = NUM_HIDDEN_UNITS[i - 1];
    }
    num_units[NUM_LAYERS + 1] = NUM_CLASSES;  // number of classes

    bool RANDOM_WEIGHTS = true;
    bool RANDOM_DATA = true;

    // We have NUM_LAYERS weight matrices, sizes are given in num_units
    // NOTE: we do not necessarily need full precision here in the weights
    // ...............
    size_t w_size = 0;                   // number of weights total
    int num_rows[NUM_LAYERS + 1];     // the sizes of each weight matrix
    int num_columns[NUM_LAYERS + 1];  // ""
    for (i = 0; i < NUM_LAYERS + 1; i++) {
        printf("Weight matrix %d has size (%d, %d)\n", i, num_units[i] + 1,
               num_units[i + 1]);
        num_columns[i] = num_units[i] + 1;  // For the bias
        num_rows[i] = num_units[i + 1];
        w_size += num_columns[i] * num_rows[i];
    }
    printf("Network has %lu weights in total.\n", w_size);

    // Initialize weights, data, and labels.
    float* weights;
    int err;
    err = posix_memalign((void**)&weights, CACHELINE_SIZE, w_size * sizeof(float));
    ASSERT_MEMALIGN(weights, err);
    init_weights(weights, w_size, RANDOM_WEIGHTS);

    float* data;
    int* labels;
    size_t data_size = NUM_TEST_CASES * INPUT_DIM;
    size_t label_size = NUM_TEST_CASES;
    err = posix_memalign(
            (void**)&data, CACHELINE_SIZE, data_size * sizeof(float));
    ASSERT_MEMALIGN(data, err);
    err = posix_memalign(
            (void**)&labels, CACHELINE_SIZE, label_size * sizeof(int));
    ASSERT_MEMALIGN(labels, err);
    init_data(data, NUM_TEST_CASES, INPUT_DIM, RANDOM_DATA);
    init_labels(labels, NUM_TEST_CASES, RANDOM_DATA);
    printf("Data has %lu elements.\n", data_size);

    // Get the dimensions of the biggest matrix that will ever come out of
    // matrix_multiply. All of them will have NUM_TEST_CASES columns. So I just
    // find the biggest number of rows.
    printf("Setting up arrays\n");
    int biggest_rows = num_units[1];
    for (i = 2; i < NUM_LAYERS + 2; i++) {
        if (num_units[i] > biggest_rows) {
            biggest_rows = num_units[i];
        }
    }
    printf("Largest hidden/output layer: %d\n", biggest_rows);
    fflush(stdout);

    // Then, allocate memory for it. We will always place the result of our
    // matrix multiplications in here.
    //
    // Mapped to its own scratchpad.
    float* hid;
    float* hid_temp;
    size_t hid_size = NUM_TEST_CASES * biggest_rows;
    err = posix_memalign(
            (void**)&hid, CACHELINE_SIZE, hid_size * sizeof(float));
    ASSERT_MEMALIGN(hid, err);
    err = posix_memalign(
            (void**)&hid_temp, CACHELINE_SIZE, hid_size * sizeof(float));
    ASSERT_MEMALIGN(hid_temp, err);

    // This file is not looked at by aladdin so malloc is fine.
    // If I do the old version then I get a memory overflow, because the
    // max stack size is not big enough for TIMIT stuff.

    // Build the sigmoid lookup table
    // May want to change this to be "non-centered"
    // to avoid (sigmoid_coarseness - 1.0)
    // so we can use bit shift in lookup function with fixed point precisions
    printf("Setting up sigmoid lookup table...\n");
    int sigmoid_coarseness = 1 << LG_SIGMOID_COARSENESS;
    float sigmoid_table[sigmoid_coarseness];
    float sig_step = (float)(SIG_MAX - SIG_MIN) / (sigmoid_coarseness - 1.0);
    float x_sig = (float)SIG_MIN;
    for (i = 0; i < sigmoid_coarseness; i++) {
        sigmoid_table[i] = conv_float2fixed(1.0 / (1.0 + exp(-x_sig)));
        // printf("%f, %f\n", x_sig, sigmoid_table[i]);
        x_sig += sig_step;
    }

    // -------------------------------------------------------- //
    //     THIS IS THE FUNCTION BEING SIMULATED IN HARDWARE     //
    // -------------------------------------------------------- //
#ifdef GEM5_HARNESS
    mapArrayToAccelerator(
            INTEGRATION_TEST, "data", data, data_size * sizeof(float));
    mapArrayToAccelerator(
            INTEGRATION_TEST, "weights", weights, w_size * sizeof(float));
    mapArrayToAccelerator(
            INTEGRATION_TEST, "hid", hid, hid_size * sizeof(float));
    mapArrayToAccelerator(
            INTEGRATION_TEST, "hid_temp", hid_temp, hid_size * sizeof(float));
    // sigmoid_table, num_units, num_rows, and num_columns I consider as
    // configuration, which is really one-time (compared to data and weights
    // which may need to be reloaded multiple times for the same network).
    // They're small enough that they should be completely partitioned, and
    // because they're configuration time parameters, I don't count them as
    // DMA.
    invokeAcceleratorAndBlock(INTEGRATION_TEST);
#else
    // Run a forward pass through the neural net
    printf("Running forward pass\n");
    nnet_fwd(data, weights, num_units, num_rows, num_columns, hid, hid_temp,
             sigmoid_table);  // The function being synthesized
#endif

    // "hid" now contains the outputs

    // Print the result, maybe not all the test_cases
    int num_to_print = 1;
    // don't try to print more test cases than there are
    num_to_print =
            num_to_print < NUM_TEST_CASES ? num_to_print : NUM_TEST_CASES;

    // Compute the classification error rate
    float* result = NUM_LAYERS % 2 == 0 ? hid : hid_temp;
    int num_errors = 0;
    for (i = 0; i < NUM_TEST_CASES; i++) {
        if (arg_max(result + i * NUM_CLASSES, NUM_CLASSES, 1) != labels[i]) {
            num_errors = num_errors + 1;
        }
    }
    FILE* output_labels = fopen("output_labels.out", "w");
    for (i = 0; i < NUM_TEST_CASES; i++) {
        fprintf(output_labels, "Test %d label: %d\n", i,
                arg_max(result + i * NUM_CLASSES, NUM_CLASSES, 1));
    }
    fclose(output_labels);
    float error_fraction = ((float)num_errors) / ((float)NUM_TEST_CASES);
    printf("Fraction incorrect (over %d cases) = %f\n", NUM_TEST_CASES,
           error_fraction);

    // Write this number to a file
    FILE* accuracy_file;
    accuracy_file = fopen("accuracy.txt", "w");
    fprintf(accuracy_file, "%f", error_fraction);
    fclose(accuracy_file);

    free(hid);
    free(hid_temp);
    free(data);
    free(labels);
}
