#ifndef LIGHTGBM_TREELEARNER_GPU_TREE_LEARNER_H_
#define LIGHTGBM_TREELEARNER_GPU_TREE_LEARNER_H_

#include <LightGBM/utils/random.h>
#include <LightGBM/utils/array_args.h>

#include <LightGBM/dataset.h>
#include <LightGBM/tree.h>
#include <LightGBM/feature_group.h>
#include "feature_histogram.hpp"
#include "serial_tree_learner.h"
#include "data_partition.hpp"
#include "split_info.hpp"
#include "leaf_splits.hpp"

#include <cstdio>
#include <vector>
#include <random>
#include <cmath>
#include <memory>
#include <boost/compute/core.hpp>
#include <boost/compute/memory/local_buffer.hpp>
#include <boost/compute/algorithm/transform.hpp>
#include <boost/compute/container/vector.hpp>
#include <boost/compute/functional/math.hpp>
#include <boost/align/aligned_allocator.hpp>


namespace LightGBM {

/*!
* \brief GPU-based parallel learning algorithm.
*/
class GPUTreeLearner: public SerialTreeLearner {
public:
  explicit GPUTreeLearner(const TreeConfig* tree_config);
  ~GPUTreeLearner();
  void Init(const Dataset* train_data) override;
  void ResetTrainingData(const Dataset* train_data) override;

  void SetBaggingData(const data_size_t* used_indices, data_size_t num_data) override {
    SerialTreeLearner::SetBaggingData(used_indices, num_data);
    // determine if we are using bagging before we construct the data partition
    // thus we can start data movement to GPU earlier
    if (used_indices != nullptr) {
      if (num_data != num_data_) {
        use_bagging_ = true;
        return;
      }
    }
    use_bagging_ = false;
  }

protected:
  void BeforeTrain() override;
  bool BeforeFindBestSplit(int left_leaf, int right_leaf) override;
  void FindBestThresholds() override;
  void Split(Tree* tree, int best_Leaf, int* left_leaf, int* right_leaf) override;
private:
  /*! \brief 4-byte feature tuple used by GPU kernels */
  struct Feature4 {
	  union {
		  unsigned char s[4];
		  struct {
			  unsigned char s0;
			  unsigned char s1;
			  unsigned char s2;
			  unsigned char s3;
		  };
	  };
  };
  
  /*! \brief Single precision histogram entiry for GPU */
  struct GPUHistogramBinEntry {
    score_t sum_gradients;
    score_t sum_hessians;
    uint32_t cnt;
  };

  /*!
  * \brief Find the best number of workgroups processing one feature for maximizing efficiency
  * \param leaf_num_data The number of data examples on the current leaf being processed
  * \return Log2 of the best number for workgroups per feature, in range 0...kMaxLogWorkgroupsPerFeature
  */
  int GetNumWorkgroupsPerFeature(data_size_t leaf_num_data);
  
  /*!
  * \brief Initialize GPU device, context and command queues
  *        Also compiles the OpenCL kernel
  * \param platform_id OpenCL platform ID
  * \param device_id OpenCL device ID
  */
  void InitGPU(int platform_id, int device_id);

  /*!
  * \brief Allocate memory for GPU computation
  */
  void AllocateGPUMemory();

  /*! 
   * \brief Compute GPU feature histogram for the current leaf.
   *        Indices, gradients and hessians have been copied to the device.
   * \param leaf_num_data Number of data on current leaf
  */
  void GPUHistogram(data_size_t leaf_num_data);
  
  /*!
   * \brief Wait for GPU kernel execution and read histogram
   * \param histograms Destination of histogram results from GPU.
  */
  template <typename HistType>
  void WaitAndGetHistograms(FeatureHistogram* histograms); 

  /*!
   * \brief Construct GPU histogram asynchronously. 
   *        Interface is similar to Dataset::ConstructHistograms().
   * \param is_feature_used An predicate vector for enabling each feature
   * \param data_indices Array of data example IDs to be included in histogram, will be copied to GPU.
   *                     Set to nullptr to skip copy to GPU.
   * \param num_data Number of data examples to be included in histogram
   * \param gradients Array of gradients for all examples.
   * \param hessians Array of hessians for all examples.
   * \param ordered_gradients Ordered gradients will be generated and copied to GPU when gradients is not nullptr, 
   *                     Set gradients to nullptr to skip copy to GPU.
   * \param ordered_hessians Ordered hessians will be generated and copied to GPU when hessians is not nullptr, 
   *                     Set hessians to nullptr to skip copy to GPU.
   * \return true if GPU kernel is launched, false if GPU is not used
  */
  bool ConstructGPUHistogramsAsync(
    const std::vector<int8_t>& is_feature_used,
    const data_size_t* data_indices, data_size_t num_data,
    const score_t* gradients, const score_t* hessians,
    score_t* ordered_gradients, score_t* ordered_hessians);

  /*! brief Log2 of max number of workgroups per feature*/
  const int kMaxLogWorkgroupsPerFeature = 10; // 2^10
  /*! brief Max total number of workgroups with preallocated workspace.
   *        If we use more than this number of workgroups, we have to reallocate subhistograms */
  int preallocd_max_num_wg_ = 1024;

  /*! \brief True if bagging is used */
  bool use_bagging_;

  /*! \brief GPU device object */
  boost::compute::device dev_;
  /*! \brief GPU context object */
  boost::compute::context ctx_;
  /*! \brief GPU command queue object */
  boost::compute::command_queue queue_;
  /*! \brief GPU kernel for 256 bins */
  const char *kernel256_src_ = 
  #include "ocl/histogram256.cl"
  ;
  /*! \brief GPU kernel for 64 bins */
  const char *kernel64_src_ = 
  #include "ocl/histogram64.cl"
  ;
  /*! \brief GPU kernel for 64 bins */
  const char *kernel16_src_ = 
  #include "ocl/histogram16.cl"
  ;

  /*! \brief a array of histogram kernels with different number
     of workgroups per feature */
  std::vector<boost::compute::kernel> histogram_kernels_;
  /*! \brief a array of histogram kernels with different number
     of workgroups per feature, and processing the whole dataset */
  std::vector<boost::compute::kernel> histogram_fulldata_kernels_;
  /*! \brief total number of dense features, which will be processed on GPU */
  int num_dense_features_;
  /*! \brief On GPU we read one DWORD (4-bypte) of features of one example once.
   *  With bin size > 16, there are 4 features per DWORD.
   *  With bin size <=16, there are 8 features per DWORD.
   * */
  int dword_features_;
  /*! \brief total number of dense feature tuples on GPU.
   * Each feature tuple is 4-byte (4 features if each feature takes a byte) */
  int num_dense_feature4_;
  /*! \brief Max number of bins of training data, used to determine 
   * which GPU kernel to use */
  int max_num_bin_;
  /*! \brief Used GPU kernel bin size (64, 256) */
  int device_bin_size_;
  /*! \brief Size of histogram bin entry, depeding if single or double precision is used */
  size_t hist_bin_entry_sz_;
  /*! \brief Indices of all dense features */
  std::vector<int> dense_feature_map_;
  /*! \brief Indices of all sparse features */
  std::vector<int> sparse_feature_map_;
  /*! \brief Multipliers of all dense features, used for redistribute bins */
  std::vector<int> device_bin_mults_;
  /*! \brief GPU memory object holding the training data */
  std::unique_ptr<boost::compute::vector<Feature4>> device_features_;
  /*! \brief GPU memory object holding the ordered gradient */
  boost::compute::buffer device_gradients_;
  /*! \brief Pinned memory object for ordered gradient */
  boost::compute::buffer pinned_gradients_;
  /*! \brief Pointer to pinned memory of ordered gradient */
  void * ptr_pinned_gradients_ = nullptr;
  /*! \brief GPU memory object holding the ordered hessian */
  boost::compute::buffer device_hessians_;
  /*! \brief Pinned memory object for ordered hessian */
  boost::compute::buffer pinned_hessians_;
  /*! \brief Pointer to pinned memory of ordered hessian */
  void * ptr_pinned_hessians_ = nullptr;
  /*! \brief GPU memory object holding indices of the leaf being processed */
  std::unique_ptr<boost::compute::vector<data_size_t>> device_data_indices_;
  /*! \brief GPU memory object holding counters for workgroup coordination */
  std::unique_ptr<boost::compute::vector<int>> sync_counters_;
  /*! \brief GPU memory object holding temporary sub-histograms per workgroup */
  std::unique_ptr<boost::compute::vector<char>> device_subhistograms_;
  /*! \brief Host memory object for histogram output (GPU will write to Host memory directly) */
  boost::compute::buffer device_histogram_outputs_;
  /*! \brief Host memory pointer for histogram outputs */
  void * host_histogram_outputs_;
  /*! \brief OpenCL waitlist object for waiting for data transfer before kernel execution */
  boost::compute::wait_list kernel_wait_obj_;
  /*! \brief OpenCL waitlist object for reading output histograms after kernel execution */
  boost::compute::wait_list histograms_wait_obj_;
  /*! \brief Asynchronous waiting object for copying indices */
  boost::compute::future<void> indices_future_;
  /*! \brief Asynchronous waiting object for copying gradients */
  boost::compute::event gradients_future_;
  /*! \brief Asynchronous waiting object for copying hessians */
  boost::compute::event hessians_future_;
};

}  // namespace LightGBM
#endif   // LightGBM_TREELEARNER_GPU_TREE_LEARNER_H_

