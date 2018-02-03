#include <stdint.h>
#include <string.h>

#include "core/nnet_fwd_defs.h"
#include "utility/compression.h"
#include "utility/utility.h"

#define MASK_AND_SHIFT(array, array_idx, vec_offset)                           \
    ((array)[array_idx] & 0xf) << (4 * (vec_offset))

// These two vectors must be of the same size.
typedef float v8fp_t __attribute__((__vector_size__(TOTAL_VECTOR_BYTES)));
typedef uint16_t v16short_t __attribute__((__vector_size__(TOTAL_VECTOR_BYTES)));

// This vector is used to manipulate the packed elements at VECTOR_SIZE
// granularity.
typedef uint16_t v8short_t
        __attribute__((__vector_size__(VECTOR_SIZE * PACKED_ELEMENT_SIZE)));

csr_array_t alloc_csr_array_t(size_t num_nonzeros, size_t num_rows) {
    csr_array_t csr;
    csr.vals = (float*)malloc_aligned(num_nonzeros * sizeof(float));
    csr.col_idx = (int*)malloc_aligned(num_nonzeros * sizeof(int));
    csr.row_idx = (int*)malloc_aligned((num_rows + 1) * sizeof(int));
    csr.num_nonzeros = num_nonzeros;
    csr.num_rows = num_rows;
    return csr;
}

csr_array_t copy_csr_array_t(csr_array_t* existing_array) {
    csr_array_t csr = alloc_csr_array_t(
            existing_array->num_nonzeros, existing_array->num_rows);
    memcpy((void*)csr.vals, (void*)existing_array->vals,
           existing_array->num_nonzeros * sizeof(float));
    memcpy((void*)csr.col_idx, (void*)existing_array->col_idx,
           existing_array->num_nonzeros * sizeof(int));
    memcpy((void*)csr.row_idx, (void*)existing_array->row_idx,
           (existing_array->num_rows + 1) * sizeof(int));
    csr.num_nonzeros = existing_array->num_nonzeros;
    csr.num_rows = existing_array->num_rows;
    return csr;
}

void free_csr_array_t(csr_array_t* ptr) {
    free(ptr->vals);
    free(ptr->col_idx);
    free(ptr->row_idx);
}

// Allocate memory to store a packed CSR array.
//
// This struct needs to be accessed as a contiguous block of memory by an
// accelerator, so we need to allocate the memory as such. The pointers in the
// struct are simply referring to locations in the middle of the block.
packed_csr_array_t alloc_packed_csr_array_t(size_t num_total_vectors,
                                            size_t num_nonzeros,
                                            size_t num_rows) {
    packed_csr_array_t csr;
    size_t values_size = next_multiple(
            num_total_vectors * TOTAL_VECTOR_BYTES, CACHELINE_SIZE);
    size_t col_idx_size = next_multiple(
            num_total_vectors * DATA_TO_INDEX_RATIO * sizeof(uint32_t),
            CACHELINE_SIZE);
    size_t row_idx_size =
            next_multiple(num_rows * sizeof(uint32_t), CACHELINE_SIZE);
    size_t total_buf_size = values_size + col_idx_size + row_idx_size;
    uint32_t* buffer = (uint32_t*)malloc_aligned(total_buf_size);
    csr.vals = buffer;
    csr.col_idx = csr.vals + values_size / sizeof(uint32_t);
    csr.row_idx = csr.col_idx + col_idx_size / sizeof(uint32_t);
    csr.num_nonzeros = num_nonzeros;
    csr.num_rows = num_rows;
    csr.total_buf_size = total_buf_size;  // Used for setting TLB mappings.
    return csr;
}

void free_packed_csr_array_t(packed_csr_array_t* ptr) {
    // There was only one memory allocation required for the entire struct.
    free(ptr->vals);
}

/* Compress an uncompressed matrix into the modified CSR format.
 *
 * The modified CSR format is based on the CSC format used in Deep
 * Compression (Han et al):
 *   1. The nonzero values are stored linearly.
 *   2. Indices are the relative offsets from the previous value to the
 *      next nonzero value position. They are represented as 4-bit values,
 *      so if any two nonzero values are spaced 16 columns or more apart,
 *      a padding zero is inserted into the data array with offset 15.
 *   3. Row indices are stored in unmodified CSR format.
 *   4. Native types are used - float for the data, int for column and row
 *      indices.
 */
csr_array_t compress_dense_data_csr(float* data, dims_t* data_dims) {
    int num_values = get_dims_size(data_dims);
    // First we'll allocate space for the complete dense array; later, once
    // we've completely compressed the array, we'll copy it into a new smaller
    // sparse array. This is because due to the limited bitwidth for relative
    // offsets, we don't know how much internal zero padding is needed.
    csr_array_t csr = alloc_csr_array_t(num_values, data_dims->rows);
    ARRAY_3D(float, _data, data, data_dims->rows, data_dims->cols);

    int num_nonzeros = 0;
    int curr_row_idx = 1;
    for (int h = 0; h < data_dims->height; h++) {
        for (int r = 0; r < data_dims->rows; r++) {
            PRINT_MSG_V("Row %d\n", r);
            // First, count the total number of nonzeros in this row.
            int num_elems_in_row = 0;
            int last_nz_idx = 0;
            for (int c = 0; c < data_dims->cols; c++) {
                if (_data[h][r][c] != 0) {
                    num_elems_in_row++;
                    last_nz_idx = c;
                }
            }
            PRINT_MSG_V("  Number of non zeros: %d, last idx: %d\n",
                        num_elems_in_row,
                        last_nz_idx);

            int next_offset = 0;
            for (int c = 0; c <= last_nz_idx; c++) {
                float curr_value = _data[h][r][c];
                if (curr_value == 0)
                    next_offset++;
                if (curr_value != 0 || next_offset == 16) {
                    if (next_offset == 16)
                        next_offset--;
                    csr.vals[num_nonzeros] = curr_value;
                    csr.col_idx[num_nonzeros] = next_offset;
                    PRINT_MSG_V(" Writing %5.5f, %d at index %d\n",
                                curr_value,
                                next_offset,
                                num_nonzeros);
                    num_nonzeros++;
                    next_offset = 0;
                }
            }
            csr.row_idx[curr_row_idx++] = num_nonzeros;
        }
    }
    csr.num_nonzeros = num_nonzeros;
    csr.row_idx[0] = 0;

    // Copy the data to a new sparse array and free the current one.
    csr_array_t result = copy_csr_array_t(&csr);
    free_csr_array_t(&csr);
    return result;
}

/* Pack data in the modified CSR format into a more compact storage format.
 *
 * The packed, quantized format looks like:
 *   1. Each value is compressed to 16 bit half precision floating point.
 *   2. 16 FP16 values are packed into 32-byte vectors.
 *   3. New rows always start on vector-aligned addresses; they cannot
 *      cross vector boundaries.
 *   4. 8 4-bit integer offsets are packed into 32-bit integers.
 *   5. Each row index is represented as a 32-bit packed pair of values.
 *      a. Bits 0-15: The number of elements in this row.
 *      b. Bits 16-31: The vector index in the data array that marks the
 *         beginning of this row.
 */
packed_csr_array_t pack_data_vec8_f16(csr_array_t csr_data, dims_t* data_dims) {
    PRINT_MSG_V("==== COMPRESSING ===== \n");
    // First, compute the overall size of the packed data, accounting for
    // row-alignment requirements.
    size_t total_num_vectors = 0;
    for (int row = 0; row < data_dims->rows; row++) {
        int num_elems_in_row =
                csr_data.row_idx[row + 1] - csr_data.row_idx[row];
        total_num_vectors += FRAC_CEIL(num_elems_in_row, DATA_PACKING_FACTOR);
    }
    PRINT_MSG_V("total num vectors: %lu\n", total_num_vectors);

    packed_csr_array_t csr = alloc_packed_csr_array_t(
            total_num_vectors, csr_data.num_nonzeros, data_dims->rows);

    v16short_t* _data = (v16short_t*)csr.vals;
    // Independently track the current linear index into the compressed data
    // column, and row indices arrays.
    int curr_wgt_src_idx = 0;
    int curr_wgt_dst_idx = 0;
    int curr_col_src_idx = 0;
    int curr_col_dst_idx = 0;
    int curr_packed_row_idx = 0;
    // Track the number of elements we've packed, so we can sanity check to
    // make sure we never exceed num_nonzero.
    unsigned total_elements_packed = 0;
    for (int row = 0; row < data_dims->rows; row++) {
        int row_start_idx = csr_data.row_idx[row + 1];
        const int num_elems_in_row = row_start_idx - csr_data.row_idx[row];
        int num_packed_data_vectors =
                FRAC_CEIL(num_elems_in_row, VECTOR_SIZE * 2);
        PRINT_MSG_V("Row = %d\n", row);
        PRINT_MSG_V("  row start idx %d\n", row_start_idx);
        PRINT_MSG_V("  num elements in row %d\n", num_elems_in_row);
        PRINT_MSG_V("  num packed vectors %d\n", num_packed_data_vectors);
        int elems_remaining = num_elems_in_row;
        for (int vec = 0; vec < num_packed_data_vectors; vec++) {
            v8fp_t data_f32 = (v8fp_t){ 0 };
            v16short_t data_f16 = (v16short_t){ 0, 0, 0, 0, 0, 0, 0, 0,
                                                0, 0, 0, 0, 0, 0, 0, 0 };
            // We can only pack up to 8 SP floats at once, but the size of the
            // vector containing the packed data that we will eventually read
            // out is the same size in bytes as the uncompressed, so multiple
            // iterations are needed to thoroughly pack all possible elements
            // into the vector.
            for (int iter = 0;
                 iter < (int)(UNPACKED_ELEMENT_SIZE / PACKED_ELEMENT_SIZE);
                 iter++) {
                if (elems_remaining <= 0)
                    break;
                for (int col = 0; col < min2(elems_remaining, VECTOR_SIZE);
                     col++) {
                    data_f32[col] = csr_data.vals[curr_wgt_src_idx++];
                }
                v8short_t packed_f16 = _CVT_PS_PH_256(data_f32, 0);
                for (int i = 0; i < VECTOR_SIZE; i++) {
                    PRINT_MSG_V("  packed data: %#4x\n", packed_f16[i]);
                    data_f16[iter * VECTOR_SIZE + i] = packed_f16[i];
                }

                elems_remaining -= VECTOR_SIZE;
            }
            PRINT_MSG_V("  Storing to data[%d]\n", curr_wgt_dst_idx);
            _data[curr_wgt_dst_idx++] = data_f16;
        }

        // 4 bit indices -> 8 per 32-bit integer.
        elems_remaining = num_elems_in_row;
        int num_packed_idx_vectors =
                num_packed_data_vectors * DATA_TO_INDEX_RATIO;
        for (int vec = 0; vec < num_packed_idx_vectors; vec++) {
            csr.col_idx[curr_col_dst_idx] = 0;
            for (int elem = 0;
                 elem < min2(elems_remaining, (int)INDEX_PACKING_FACTOR);
                 elem++) {
                csr.col_idx[curr_col_dst_idx] |= MASK_AND_SHIFT(
                        csr_data.col_idx, curr_col_src_idx++, elem);
            }
            PRINT_MSG_V("  packed col_idx[%d] = %#x\n",
                        curr_col_dst_idx,
                        csr.col_idx[curr_col_dst_idx]);
            elems_remaining -= INDEX_PACKING_FACTOR;
            curr_col_dst_idx++;
        }

        csr.row_idx[row] = ((curr_packed_row_idx << 16) & 0xffff0000) |
                           (num_elems_in_row & 0xffff);
        curr_packed_row_idx += num_packed_data_vectors;
        PRINT_MSG_V("  packed row = %#x\n", csr.row_idx[row]);
        total_elements_packed += num_elems_in_row;
    }
    assert(total_elements_packed == csr_data.num_nonzeros &&
           "The number of packed elements is not the same as the number of non "
           "zero elements specified!");

    PRINT_MSG_V("Compressed data:\n");
    for (unsigned i = 0; i < total_num_vectors; i++)
        PRINT_MSG_V("%#x ", csr.vals[i]);
    PRINT_MSG_V("\nCompressed col indices:\n");
    for (unsigned i = 0; i < total_num_vectors * DATA_TO_INDEX_RATIO; i++)
        PRINT_MSG_V("%#x ", csr.col_idx[i]);
    PRINT_MSG_V("\nCompressed row indices:\n");
    for (int i = 0; i < data_dims->rows; i++)
        PRINT_MSG_V("%#x ", csr.row_idx[i]);
    PRINT_MSG_V("\n");

    return csr;
}

// Decompress data in unpacked modified CSR format.
void decompress_csr_data(csr_array_t* csr_data,
                         dims_t* data_dims,
                         float* dcmp_data) {
    int data_rows = data_dims->rows;
    int data_cols = data_dims->cols;
    int data_pad = data_dims->align_pad;

    ARRAY_2D(float, _data, dcmp_data, data_cols + data_pad);
    PRINT_MSG_V("==== DECOMPRESSING ==== \n");
    int curr_col_idx = 0;
    for (int row = 0; row < data_rows; row++) {
        int curr_row_start_idx = csr_data->row_idx[row];
        int next_row_start_idx = csr_data->row_idx[row + 1];
        int num_elems_in_row = next_row_start_idx - curr_row_start_idx;
        PRINT_MSG_V("Row %d\n", row);
        PRINT_MSG_V("  Row start idx: %d\n", curr_row_start_idx);
        PRINT_MSG_V("  Row size: %d\n", num_elems_in_row);

        // A column index of zero means there are no zeros in between it and
        // the previous nonzero value.  So, we need to implicitly add 1 to the
        // existing offset to get the new decompressed column index. This
        // boundary condition is easily handled with a starting offset of -1.
        int col_idx = 0;
        for (int col = 0; col < num_elems_in_row; col++) {
            col_idx += csr_data->col_idx[curr_col_idx];
            ASSERT(col_idx < (data_cols + data_pad) &&
                   "Column index exceeds width of matrix!");
            float value = csr_data->vals[curr_col_idx];
            _data[row][col_idx] = value;
            curr_col_idx++;
            col_idx++;
            PRINT_MSG_V("  Storing _data[%d][%d] = %f\n", row, col_idx, value);
        }
    }
}

// Unpack a vector's worth of CSR values and indices at a specific location.
//
// Since a vector stores 16 FP16 elements, this returns the unpacked
// single-precision values and indices at that location in values_buffer and
// index_buffer.
//
// Args:
//   cmp_values: A pointer to the start of the packed CSR data.
//   cmp_col_idx: A pointer to the start of the packed CSR column indices.
//   fetch_index_vec: The index of the vector to fetch from the two arrays
//                    above. This index refers to a VECTOR_ALIGNED memory
//                    address.
//   values_buffer: Stores up to 16 unpacked values.
//   index_buffer: Stores up to 16 unpacked indices.
void unpack_values_at_row(uint32_t* cmp_values,
                          uint32_t* cmp_col_idx,
                          int fetch_index_vec,
                          float values_buffer[VECTOR_SIZE * 2],
                          int index_buffer[VECTOR_SIZE * 2]) {
    v16short_t* _cmp_values = (v16short_t*)cmp_values;

    // Extract and decompress the values.
    PRINT_MSG_V("  Fetching packed values from %d\n", fetch_index_vec);
    v16short_t curr_values = _cmp_values[fetch_index_vec];

#ifdef __clang__
    v8short_t values0_f16 = __builtin_shufflevector(
            curr_values, curr_values, 0, 1, 2, 3, 4, 5, 6, 7);
    v8short_t values1_f16 = __builtin_shufflevector(
            curr_values, curr_values, 8, 9, 10, 11, 12, 13, 14, 15);
#else
    v8short_t values0_f16 =
            (v8short_t){ curr_values[0], curr_values[1], curr_values[2],
                         curr_values[3], curr_values[4], curr_values[5],
                         curr_values[6], curr_values[7] };
    v8short_t values1_f16 =
            (v8short_t){ curr_values[8],  curr_values[9],  curr_values[10],
                         curr_values[11], curr_values[12], curr_values[13],
                         curr_values[14], curr_values[15] };
#endif
    v8fp_t values0_f32 = _CVT_PH_PS_256(values0_f16);
    v8fp_t values1_f32 = _CVT_PH_PS_256(values1_f16);

    // Extract the 4-bit compressed indices.
    unsigned idx0 = cmp_col_idx[fetch_index_vec * DATA_TO_INDEX_RATIO];
    unsigned idx1 = cmp_col_idx[fetch_index_vec * DATA_TO_INDEX_RATIO + 1];

    for (int j = 0; j < VECTOR_SIZE; j++) {
        values_buffer[j] = values0_f32[j];
        index_buffer[j] = (idx0 >> (j * INDEX_BITS)) & 0xf;
    }
    for (int j = 0; j < VECTOR_SIZE; j++) {
        values_buffer[j + VECTOR_SIZE] = values1_f32[j];
        index_buffer[j + VECTOR_SIZE] = (idx1 >> (j * INDEX_BITS)) & 0xf;
    }
}

// Directly decompress data stored in a packed variation of CSR.
//
// cmp_data: Compressed data, packed in groups of 16 x FP16 elements.
// cmp_col_idx: Relative 4-bit indices that indicate the number of zeros before
//              the next value in cmp_values in the same row.
// cmp_row_idx: Packed pair of values for each row in the matrix. The first 16
//              bits indicate the starting index of the ith row (by 256 bit
//              granularity), and the second 16 bits indicate the number of
//              nonzero values in this row.
// data_dims: The dimensions of the uncompressed data.
// dcmp_data: The base of the uncompressed data buffer.
void decompress_packed_csr_data(uint32_t* cmp_data,
                                uint32_t* cmp_col_idx,
                                uint32_t* cmp_row_idx,
                                dims_t* data_dims,
                                float* dcmp_data) {
    int data_rows = data_dims->rows;
    int data_cols = data_dims->cols;
    int data_pad = data_dims->align_pad;

    ARRAY_2D(float, _data, dcmp_data, data_cols + data_pad);
    PRINT_MSG_V("==== DECOMPRESSING ==== \n");
    for (int row = 0; row < data_rows; row++) {
        // Row indices are themselves packed into an index and the number of
        // nonzeros in that row, 16 bits each. The index indicates where the
        // first element of this row is stored in the compressed data (as a
        // 32-byte vector index). We also need the number of nonzeros stored
        // separately in order to properly handle the fact that separate rows
        // cannot cross vector (32-byte) boundaries.
        uint32_t packed_idx_size = cmp_row_idx[row];
        int curr_row_start_idx = (packed_idx_size >> 16) & 0xffff;
        int curr_row_size = packed_idx_size & 0xffff;
        PRINT_MSG_V("Row %d\n", row);
        PRINT_MSG_V("  Row start idx: %d\n", curr_row_start_idx);
        PRINT_MSG_V("  Row size: %d\n", curr_row_size);

        // A column index of zero means there are no zeros in between these two
        // nonzero values. We therefore need to implicitly add 1 to the
        // existing offset to figure out where to put the new value, and this
        // boundary condition is easily handled with a starting offset of -1.
        int col_idx = -1;
        int num_elems_remaining = curr_row_size;
        for (int col = 0; col < curr_row_size; col += DATA_PACKING_FACTOR) {
            float values_buffer[VECTOR_SIZE * 2];
            int index_buffer[VECTOR_SIZE * 2];
            unpack_values_at_row(
                    cmp_data,
                    cmp_col_idx,
                    curr_row_start_idx + (col / DATA_PACKING_FACTOR),
                    values_buffer,
                    index_buffer);
            for (int i = 0; i < (int)DATA_PACKING_FACTOR; i++) {
                PRINT_MSG_V("  values_buffer[%d] = %f, index_buffer[%d] = %d\n",
                            i, values_buffer[i], i, index_buffer[i]);
            }
            for (int val = 0; val < min2(num_elems_remaining, 16); val++) {
                float value = values_buffer[val];
                // Within each row, the column indices must be accumulated, as
                // they are relative positions, not absolute positions.
                col_idx += index_buffer[val] + 1;
                ASSERT(col_idx < data_cols + data_pad &&
                       "Column index exceeds width of matrix!");
                _data[row][col_idx] = value;
                PRINT_MSG_V(
                        "  Storing _data[%d][%d] = %f\n", row, col_idx, value);
            }
            num_elems_remaining -= 16;
        }
    }
}