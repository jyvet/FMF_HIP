/*
:copyright:
    William B. Frank and Eric Beauce
:license:
    GNU General Public License, Version 3
    (https://www.gnu.org/licenses/gpl-3.0.en.html)
*/

#include <hip/hip_runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <omp.h>
#define gpuErrchk(ans)                        \
    {                                         \
        gpuAssert((ans), __FILE__, __LINE__); \
    }

#define BLOCKSIZE 512
#define WARPSIZE 32
#define NCHUNKS 20
#define STABILITY_THRESHOLD 0.000001f
#define MEGABYTES pow(1024, 2)

#define GPUSMAX 8 

extern "C"
{ // needed for C-style symbols in shared object compiled by nvcc
#include "matched_filter_GPU.h"

    //-------------------------------------------------------------------------
    inline void gpuAssert(hipError_t code, const char *file, int line, bool abort = true)
    {

        if (code != hipSuccess)
        {
            fprintf(stderr, "An error occured in the kernel: %s %s %d\n", hipGetErrorString(code), file, line);
            if (abort)
                exit(code);
        }
    }

    //-------------------------------------------------------------------------
    __global__ void network_corr(const float * __restrict__ templates, const float * __restrict__ sum_square_template,
                                 const int * __restrict__ moveout, const float * __restrict__ data, const float * __restrict__ weights,
                                 const size_t step, const size_t n_samples_template, const size_t n_samples_data,
                                 const size_t n_stations, const size_t n_components,
                                 const size_t chunk_offset, const size_t chunk_size,
                                 float *cc_mat, const int normalize)
    {

        // each thread matches the template to one time in the data
        size_t idx, first_sample_block, first_sample_trace, last_sample_trace; // sample's index
        size_t s;                                                              // counters
        size_t data_offset, templates_offset, sum_square_template_offset, cc_mat_offset;
        float numerator, denominator, sum_square_data, mean_data;
        float data_sample;
        size_t t_idx;

        //------------------------------------------------
        size_t count_template = (n_samples_template / WARPSIZE + 1) * WARPSIZE;
        extern __shared__ float shared[];
        float *ss_template = &shared[0];
        float *templates_s = &shared[sizeof(float)];
        float *data_s = &shared[count_template + sizeof(float)];

        // 1 block processes one channel to blockDim.x / step different positions in time
        // idx is in units of correlation time
        // first_samples_block is in units of waveform time
        idx = blockIdx.x / n_stations * blockDim.x + chunk_offset;
        first_sample_block = idx * step;
        s = blockIdx.x % n_stations;

        for (size_t c = 0; c < n_components; c++)
        {
            if (weights[s * n_components + c] != 0.)
            {
                // compute offsets for input variables
                cc_mat_offset = (first_sample_block / step + threadIdx.x - chunk_offset) * n_stations * n_components + s * n_components + c;
                templates_offset = s * n_samples_template * n_components + c * n_samples_template;
                sum_square_template_offset = s * n_components + c;
                first_sample_trace = first_sample_block + moveout[s * n_components + c];
                last_sample_trace = first_sample_trace + n_samples_template + threadIdx.x * step;
                data_offset = s * n_samples_data * n_components + c * n_samples_data + first_sample_trace;

                // initialize sums
                sum_square_data = 0.0f;
                mean_data = 0.0f;
                numerator = 0.0f;

                // load template and data into shared memory
                t_idx = threadIdx.x;
                if (t_idx == 0)
                {
                    ss_template[0] = sum_square_template[sum_square_template_offset];
                }

                while (t_idx < (blockDim.x * step + n_samples_template))
                {
                    if (t_idx < n_samples_template)
                        templates_s[t_idx] = templates[templates_offset + t_idx];

                    if ((first_sample_trace + t_idx) < n_samples_data)
                        data_s[t_idx] = data[data_offset + t_idx];
                        
                    t_idx += blockDim.x;
                }

                __syncthreads(); // make sure the waveforms are read before keep going

                // calculate correlation coefficient
                if (last_sample_trace <= n_samples_data)
                {
                    // if not, corresponds to an ill-defined CC with some samples out of the bounds
                    // Calculate the mean if fully normalising
                    if (normalize > 0)
                    {
                        for (size_t i = 0; i < n_samples_template; i++)
                        {
                            mean_data += data_s[i + threadIdx.x * step];
                        }
                        mean_data /= n_samples_template;
                    }

                    for (size_t i = 0; i < n_samples_template; i++)
                    {
                        data_sample = data_s[i + threadIdx.x * step] - mean_data;
                        numerator += data_sample * templates_s[i];
                        sum_square_data += data_sample * data_sample;
                    }
                    // denominator = sum_square_data * sum_square_template[sum_square_template_offset];
                    denominator = sum_square_data * ss_template[0];
                    if (cc_mat_offset < (chunk_size * n_stations * n_components))
                    {
                        // check that this thread is not ouf of the chunk's bounds
                        if (denominator > STABILITY_THRESHOLD)
                        {
                            cc_mat[cc_mat_offset] = numerator * rsqrtf(denominator);
                        }
                    }
                }
            }
            __syncthreads(); // wait for every thread to finish before leaving the kernel
        }
    }

    //-------------------------------------------------------------------------
    __global__ void sum_cc(float *cc_mat, float *cc_sum, float *weights,
                           size_t n_stations, size_t n_components, size_t n_corr,
                           size_t chunk_offset, size_t chunk_size)
    {

        size_t i;

        i = blockIdx.x * blockDim.x + threadIdx.x;
        if (((i + chunk_offset) < n_corr) & (i < chunk_size))
        {
            // first condition: check if we are not outside cc_sum's length
            // second condition: check if we are not outside the chunk's size
            float *cc_mat_offset;

            cc_mat_offset = cc_mat + i * n_stations * n_components;
            for (size_t ch = 0; ch < (n_stations * n_components); ch++)
            {
                cc_sum[i] += cc_mat_offset[ch] * weights[ch];
            }
        }
    }

    size_t check_sharedMem(const int gpu_id, const size_t n_samples_template, const size_t step)
    {
        const size_t Mb = MEGABYTES;
        static hipDeviceProp_t props[GPUSMAX];
        static bool props_init[GPUSMAX] = { 0 };

        if (props_init[gpu_id] == false)
        {
            hipGetDeviceProperties(&props[gpu_id], gpu_id);
            props_init[gpu_id] = true;
        }

        // calculate the space required in the shared memory
        const size_t count_template = (n_samples_template / WARPSIZE + 1) * WARPSIZE;
        const size_t count_data = ((n_samples_template + BLOCKSIZE * step) / WARPSIZE + 1) * WARPSIZE;
        const size_t sharedMem = (count_template + count_data + 1) * sizeof(float);
        const int maxSharedMem = props[gpu_id].sharedMemPerBlock;
        if (sharedMem > maxSharedMem)
        {
            size_t new_step = (maxSharedMem / sizeof(float) - 2 * n_samples_template - 2 * WARPSIZE) / BLOCKSIZE;
            int new_length = maxSharedMem / sizeof(float) - count_data - WARPSIZE;
            if (new_length < 0)
                new_length = 0;
            printf("The maximum shared memory available on this card is %zu Mb "
                   "(%zu Mb required). You should consider the different options:\n"
                   "  - Change the temporal step to %zu without changing the template length.\n"
                   "  - Change the template length to %d without changing the temporal step.\n"
                   "  - Try to decrease both of these parameters.\n",
                   maxSharedMem / Mb, sharedMem / Mb, new_step, new_length);
            exit(0);
        }

        return sharedMem;
    }

typedef struct gpu_template
{
    int    id;
    float *cc_mat_d;
    float *cc_out_d;
    float *templates_d;
    int   *moveouts_d;
    float *sum_square_templates_d;
    float *weights_d;
    float *data_d;
    hipStream_t streams[NCHUNKS];
} gpu_template_t;

    //-------------------------------------------------------------------------
    void matched_filter(float *templates, float *sum_square_templates,
                        int *moveouts, float *data, float *weights, size_t step,
                        size_t n_samples_template, size_t n_samples_data,
                        size_t n_templates, size_t n_stations,
                        size_t n_components, size_t n_corr,
                        float *cc_out, int normalize, int sum_cc_mode)
    {
        const size_t Mb = MEGABYTES;
        int t_global = -1;
        size_t sizeof_cc_out = 0;
        size_t sizeof_cc_out_chunk = 0;

        size_t chunk_size = n_corr / NCHUNKS + 1;

        // Size of variables to create on the device (GPU)
        size_t sizeof_templates = sizeof(float) * n_samples_template * n_stations * n_components * n_templates;
        size_t sizeof_moveouts = sizeof(int) * n_components * n_stations * n_templates;
        size_t sizeof_data = sizeof(float) * n_samples_data * n_stations * n_components;
        size_t sizeof_cc_mat = sizeof(float) * chunk_size * n_stations * n_components; // cc matrix for one template (and one chunk of data)
        if (sum_cc_mode > 0)
        {
            // return summed CC time series (memory efficient)
            sizeof_cc_out = sizeof(float) * chunk_size; // cc sums for one template (and one chunk of data)
        }
        else
        {
            // return one CC time series per channel
            sizeof_cc_out = sizeof_cc_mat;
        }
        size_t sizeof_sum_square_templates = sizeof(float) * n_templates * n_stations * n_components;
        size_t sizeof_weights = sizeof(float) * n_templates * n_stations * n_components;
        size_t sizeof_total = sizeof_templates + sizeof_moveouts + sizeof_data + sizeof_cc_mat + sizeof_cc_out + sizeof_sum_square_templates + sizeof_weights;

        int id;

        // Card-dependent settings: prefer L1 cache or shared memory
        hipDeviceSetCacheConfig(hipFuncCachePreferShared);
        // cudaDeviceSetCacheConfig(cudaFuncCachePreferL1);

        // check if enough memory is available
        size_t freeMem = 0;
        size_t totalMem = 0;
        hipMemGetInfo(&freeMem, &totalMem);
        if (sizeof_total > freeMem)
        {
            printf("%zu Mb are requested on GPU #%i whereas it has only %zu free Mb.\n",
                   sizeof_total / Mb, id, freeMem / Mb);
            printf("Reduce the number of templates or stations processed in one batch.\n");
            exit(0);
        }

        // allocate GPU memory
        gpu_template_t gpu = { 0 };
        hipMalloc((void **)&gpu.data_d, sizeof_data);

        hipMallocAsync((void **)&gpu.cc_mat_d, sizeof_cc_mat, NULL);
        hipMallocAsync((void **)&gpu.cc_out_d, sizeof_cc_out, NULL);

        const size_t bytes = n_stations * n_components * sizeof(float);
        hipMallocAsync((void **)&gpu.moveouts_d, bytes, NULL);
        hipMallocAsync((void **)&gpu.weights_d, bytes, NULL);
        hipMallocAsync((void **)&gpu.sum_square_templates_d, bytes, NULL);
        hipMallocAsync((void **)&gpu.templates_d, bytes * n_samples_template, NULL);

        hipMemcpyAsync(gpu.data_d, data, sizeof_data, hipMemcpyHostToDevice, NULL);

        const size_t sharedMem = check_sharedMem(gpu.id, n_samples_template, step);

        for (size_t ch = 0; ch < NCHUNKS; ch++)
            hipStreamCreateWithFlags(&gpu.streams[ch], hipStreamNonBlocking);

        hipDeviceSynchronize();

        // loop over templates
        for (size_t t = 0; t < n_templates; t++)
        {       
            hipMemsetAsync(gpu.cc_mat_d, 0, sizeof_cc_mat, NULL);

            hipMemcpyAsync(gpu.sum_square_templates_d, &sum_square_templates[t * n_stations * n_components], bytes, hipMemcpyHostToDevice, NULL);
            hipMemcpyAsync(gpu.moveouts_d, &moveouts[t * n_stations * n_components], bytes, hipMemcpyHostToDevice, NULL);
            hipMemcpyAsync(gpu.weights_d, &weights[t * n_stations * n_components], bytes, hipMemcpyHostToDevice, NULL);
            hipMemcpyAsync(gpu.templates_d, &templates[t * n_samples_template * n_stations * n_components], bytes * n_samples_template, hipMemcpyHostToDevice, NULL);

            size_t n_corr_t;
            int max_moveout;
            float *templates_d_t = NULL;
            int *moveouts_t = NULL, *moveouts_d_t = NULL;
            float *cc_out_t = NULL;
            float *sum_square_templates_d_t = NULL;
            float *weights_d_t = NULL;

            // compute the number of correlation steps for this template
            moveouts_t = moveouts + t * n_stations * n_components;
            max_moveout = 0;

            //#pragma omp parallel for 
            for (size_t i = 0; i < (n_stations * n_components); i++)
            {
                max_moveout = (moveouts_t[i] > max_moveout) ? moveouts_t[i] : max_moveout;
            }
            n_corr_t = (n_samples_data - n_samples_template - max_moveout) / step + 1;

            hipDeviceSynchronize();

            //#pragma omp parallel for
            for (size_t ch = 0; ch < NCHUNKS; ch++)
            {
                size_t chunk_offset = ch * chunk_size;
                size_t cs;
                // make sure the chunk is not going out of bounds
                if (chunk_offset + chunk_size > n_corr_t)
                {
                    cs = n_corr_t - chunk_offset;
                    if (cs <= 0)
                        continue;
                }
                else
                {
                    cs = chunk_size;
                }

                // define block and grid sizes for kernels
                dim3 BS(BLOCKSIZE);
                dim3 GS(ceilf(cs / (float)BS.x) * n_stations);

                    // process
                hipLaunchKernelGGL(network_corr, dim3(GS), dim3(BS), sharedMem, gpu.streams[ch], gpu.templates_d,
                                                    gpu.sum_square_templates_d,
                                                    gpu.moveouts_d,
                                                    gpu.data_d,
                                                    gpu.weights_d,
                                                    step,
                                                    n_samples_template,
                                                    n_samples_data,
                                                    n_stations,
                                                    n_components,
                                                    chunk_offset,
                                                    cs,
                                                    gpu.cc_mat_d,
                                                    normalize);

                // return an error if something happened in the kernel (and crash the program)
                //gpuErrchk(hipPeekAtLastError());

                if (sum_cc_mode > 0)
                {
                    // weighted sum of correlation coefficients
                    hipMemsetAsync(gpu.cc_out_d, 0, sizeof_cc_out, gpu.streams[ch]);

                    // using a small block size seems to improve the speed of sum_cc
                    dim3 BS_sum(32);
                    dim3 GS_sum(ceilf(cs / (float)BS_sum.x));
                    hipLaunchKernelGGL(sum_cc, dim3(GS_sum), dim3(BS_sum), 0, gpu.streams[ch], gpu.cc_mat_d, gpu.cc_out_d, gpu.weights_d,
                                               n_stations, n_components,
                                               n_corr_t, chunk_offset, cs);

                    // return an error if something happened in the kernel (and crash the program)
                    //gpuErrchk(hipPeekAtLastError());

                    // xfer cc_sum back to host
                    sizeof_cc_out_chunk = sizeof(float) * cs;
                    cc_out_t = cc_out + t * n_corr + chunk_offset;
                    hipMemcpyAsync(cc_out_t, gpu.cc_out_d, sizeof_cc_out_chunk, hipMemcpyDeviceToHost, gpu.streams[ch]);
                }
                else
                {
                    // xfer cc_mat back to host
                    sizeof_cc_out_chunk = sizeof(float) * cs * n_stations * n_components;
                    cc_out_t = cc_out + (t * n_corr + chunk_offset) * n_stations * n_components;
                    hipMemcpyAsync(cc_out_t, gpu.cc_mat_d, sizeof_cc_out_chunk, hipMemcpyDeviceToHost, gpu.streams[ch]);
                }
            }

            hipDeviceSynchronize();
        }

        for (size_t ch = 0; ch < NCHUNKS; ch++)
            hipStreamDestroy(gpu.streams[ch]);

        hipFreeAsync(gpu.cc_mat_d, NULL);
        hipFreeAsync(gpu.cc_out_d, NULL);
        hipFreeAsync(gpu.sum_square_templates_d, NULL);
        hipFreeAsync(gpu.weights_d, NULL);
        hipFreeAsync(gpu.templates_d, NULL);
        hipFreeAsync(gpu.moveouts_d, NULL);
        hipFreeAsync(gpu.data_d, NULL);
    }     //  matched_filter
} // extern C
