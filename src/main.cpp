#include <iostream>
#include <fstream>
#include <iomanip>
#include <vector>

#include <thread>
#include <chrono>
#include <cstdio>
#include <functional>
#include <algorithm>
#include <utility>

#include "global.h"
#include "riemann.h"
#include "rbarrier.h"

using namespace std;
using namespace std::chrono;

RBarrier rbarrier;
map<int, unique_ptr<mutex>> mutex_map;

void get_global_total (vector<Riemann> &thread_data_vector) {
    double sum = 0.0;
    for (int i = 0; i < thread_data_vector.size(); i++) {
        sum += thread_data_vector[i].get_local_sum();
    }
    cout << "The integral is: " << sum;
}

void get_total (vector<Riemann> &thread_data_vector, int index) {
    Riemann * current_thread = &thread_data_vector.at(index);
    short tid = current_thread->get_thread_id();
    
    high_resolution_clock::time_point start;
    if (tid == 0) 
        start = high_resolution_clock::now();
        
    current_thread->do_work();
    
    rbarrier.rbarrier_wait(
        [&current_thread, &thread_data_vector] (void)->bool {
            return current_thread->get_sharing_condition(thread_data_vector);
        } ,
        [&current_thread, &thread_data_vector] (void) {
            current_thread->callback(thread_data_vector);
        } );
        
    if (tid == 0) {
        high_resolution_clock::time_point end = high_resolution_clock::now();
        duration<double> runtime = duration_cast<duration<double>>(end - start);
        cout << "Work Time (in secs): " << runtime.count() << endl;
    }
}

int main(int argc, char * argv[]) {
    ifstream instream;
    
    if(argc != 5) {
        cout << "Not enough arguments.\n Requires [input file] "
             << "[number of threads] [dist_multiplier] [sharing flag].\n";
        return -1;
    }
    
    string input_file = argv[1];
    instream.open(input_file.c_str());
    if(!instream.is_open()) {
        cout << "Could not open file" << input_file << endl;
        return -1;
    }
    
    int l_bound, r_bound, partition_sz;
    instream >> l_bound >> r_bound >> partition_sz;
    
    int num_threads = 0;
    if(atoi(argv[2]) == 0) num_threads = 1;
    else if(atoi(argv[2]) > thread::hardware_concurrency()) {
        cout << "num_threads set to maximum supported concurrent threads"
             << " (" << thread::hardware_concurrency() << ") \n";
        num_threads = thread::hardware_concurrency() - 1;
    }
    else num_threads = atoi(argv[2]);
    
    if(num_threads > partition_sz) num_threads = partition_sz;
    
    const int dist_multiplier = atoi(argv[3]);
    const bool can_share = atoi(argv[4]);
    
    const double width = (r_bound - l_bound) / (double)partition_sz;
    
    int rc = rbarrier.rbarrier_init(num_threads);
    rbarrier.barrier_rc(rc);
    
    vector<Riemann> thread_data_vector(num_threads);
    for(int i = 0; i < num_threads; ++i) {
        thread_data_vector.at(i).thread_data_init(num_threads, can_share);
    }
    
    vector<thread> threads(num_threads);
    
    int remaining_parts = 0;
    if (partition_sz % (num_threads + dist_multiplier))
        remaining_parts = partition_sz % (num_threads - 1);
    
    double normal_dist = 0.0, init_dist = 0.0;
    if (dist_multiplier == 0 || num_threads == 1) normal_dist = partition_sz / num_threads;
    else {
        init_dist = dist_multiplier * (partition_sz / (num_threads + dist_multiplier));
        normal_dist = (partition_sz - init_dist) / (num_threads - 1);
    }
    
    double ext_dist = normal_dist + 1;
    int num_norm_parts = partition_sz - remaining_parts;
    int num_ext_parts = partition_sz - num_norm_parts;
    
    int index = 0, i = 0, j = 0;
    if (num_threads == 1) {
        thread_data_vector[index].set_thread_id(index);
        thread_data_vector[index].set_bounds(l_bound, r_bound);
        thread_data_vector[index].set_curr_location(l_bound);
        thread_data_vector[index].set_working_partitions(normal_dist);
        thread_data_vector[index].set_width(width);
        mutex_map.emplace(index, make_unique<mutex>()).first;

        high_resolution_clock::time_point start;
        start = high_resolution_clock::now();
        
        thread_data_vector[index].do_work();
        
        high_resolution_clock::time_point end = high_resolution_clock::now();
        duration<double> runtime = duration_cast<duration<double>>(end - start);
        cout << "Work Time (in secs): " << runtime.count() << endl;
    }
    else {
        for (i = 0; index < num_threads - remaining_parts; index++)
        {
            thread_data_vector[index].set_thread_id(index);
            if (dist_multiplier && (i == 0)) {
                double local_lbound = l_bound + (width * init_dist * index);
                double local_rbound = l_bound + (width * init_dist * (index + 1));
                
                thread_data_vector[index].set_bounds(local_lbound, local_rbound);
                thread_data_vector[index].set_curr_location(local_lbound);
                thread_data_vector[index].set_working_partitions(init_dist);
                i += init_dist;
            }
            else if (dist_multiplier) {
                double local_lbound = l_bound + (width * normal_dist * index) + (width * (init_dist - normal_dist));
                double local_rbound = l_bound + (width * normal_dist * (index + 1)) + (width * (init_dist - normal_dist));
                
                thread_data_vector[index].set_bounds(local_lbound, local_rbound);
                thread_data_vector[index].set_curr_location(local_lbound);
                thread_data_vector[index].set_working_partitions(normal_dist);
                i += normal_dist;
            }
            else {
                double local_lbound = l_bound + (width * normal_dist * index);
                double local_rbound = l_bound + (width * normal_dist * (index + 1));
                
                thread_data_vector[index].set_bounds(local_lbound, local_rbound);
                thread_data_vector[index].set_curr_location(local_lbound);
                thread_data_vector[index].set_working_partitions(normal_dist);
                i += normal_dist;
            }
            thread_data_vector[index].set_width(width);
            mutex_map.emplace(index, make_unique<mutex>()).first;
            threads[index] = thread(get_total, ref(thread_data_vector), index);
        }
        for (j = 0; j < num_ext_parts; i += ext_dist, j++, index++)
        {
            thread_data_vector[index].set_thread_id(index);
            thread_data_vector[index].set_bounds(l_bound + (width * ext_dist * index) + (width * (init_dist - normal_dist)),
                                                 l_bound + (width * ext_dist * (index + 1)) + (width * (init_dist - normal_dist)));
            thread_data_vector[index].set_curr_location(l_bound + (width * normal_dist * index) + (width * (init_dist - normal_dist)));
            thread_data_vector[index].set_working_partitions(ext_dist);
            thread_data_vector[index].set_width(width);
            
            mutex_map.emplace(index, make_unique<mutex>()).first;
            threads[index] = thread(get_total, ref(thread_data_vector), index);
        }
    }
    if(num_threads != 1)
        for_each(threads.begin(), threads.end(), mem_fn(&thread::join));
    
    get_global_total(ref(thread_data_vector));
    
    return EXIT_SUCCESS;
}