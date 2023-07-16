#pragma once

#include <assert.h>
#include <omp.h>
#include <stdlib.h>

#include <atomic>
#include <list>
#include <random>
#include <unordered_map>
#include <unordered_set>

#include "hnswlib.h"
#include "visited_list_pool.h"

namespace hnswlib {
typedef unsigned int tableint;
typedef unsigned int linklistsizeint;

template <typename dist_t>
class HierarchicalNSW : public AlgorithmInterface<dist_t> {
   public:
    static const tableint MAX_LABEL_OPERATION_LOCKS = 65536;
    static const unsigned char DELETE_MARK = 0x01;

    size_t max_elements_{0};
    mutable std::atomic<size_t> cur_element_count{0};  // current number of elements
    size_t size_data_per_element_{0};
    size_t size_links_per_element_{0};
    mutable std::atomic<size_t> num_deleted_{0};  // number of deleted elements
    size_t M_{0};
    size_t maxM_{0};
    size_t maxM0_{0};
    size_t ef_construction_{0};
    size_t ef_{0};

    double mult_{0.0}, revSize_{0.0};
    int maxlevel_{0};

    VisitedListPool *visited_list_pool_{nullptr};

    // Locks operations with element by label value
    mutable std::vector<std::mutex> label_op_locks_;

    std::mutex global;
    std::vector<std::mutex> link_list_locks_;

    tableint enterpoint_node_{0};

    size_t size_links_level0_{0};
    size_t offsetData_{0}, offsetLevel0_{0}, label_offset_{0};

    char *data_level0_memory_{nullptr};     // only stores links/labels now- no data
    char **linkLists_{nullptr};             // stores upper-layer links
    std::vector<int> element_levels_;       // keeps level of each element

    // MODIFIED: POINTER TO HOLD ALL DATA. OWNED BY PYTHON.
    size_t data_size_{0};
    char* data_pointer_{nullptr};   

    // distance function from space
    DISTFUNC<dist_t> fstdistfunc_;
    void *dist_func_param_{nullptr};

    mutable std::mutex label_lookup_lock;  // lock for label_lookup_
    std::unordered_map<labeltype, tableint> label_lookup_;

    std::default_random_engine level_generator_;
    std::default_random_engine update_probability_generator_;

    mutable std::atomic<long> metric_distance_computations{0};
    mutable std::atomic<long> metric_hops{0};

    // deleted functionality removed...
    bool allow_replace_deleted_ = false;  // flag to replace deleted elements (marked as deleted) during insertions
    std::mutex deleted_elements_lock;               // lock for deleted_elements
    std::unordered_set<tableint> deleted_elements;  // contains internal ids of deleted elements

    HierarchicalNSW(SpaceInterface<dist_t> *s) {}

    HierarchicalNSW(SpaceInterface<dist_t> *s, const std::string &location, bool nmslib = false,
                    size_t max_elements = 0, bool allow_replace_deleted = false)
        : allow_replace_deleted_(allow_replace_deleted) {
        loadIndex(location, s, max_elements);
    }

    HierarchicalNSW(SpaceInterface<dist_t> *s, size_t max_elements, size_t M = 16, size_t ef_construction = 200,
                    size_t random_seed = 100, bool allow_replace_deleted = false)
        : link_list_locks_(max_elements),
          label_op_locks_(MAX_LABEL_OPERATION_LOCKS),
          element_levels_(max_elements),
          allow_replace_deleted_(allow_replace_deleted) {
        max_elements_ = max_elements;
        num_deleted_ = 0;
        data_size_ = s->get_data_size();
        fstdistfunc_ = s->get_dist_func();
        dist_func_param_ = s->get_dist_func_param();
        M_ = M;
        maxM_ = M_;
        maxM0_ = M_ * 2;
        ef_construction_ = std::max(ef_construction, M_);
        ef_ = 10;

        level_generator_.seed(random_seed);
        update_probability_generator_.seed(random_seed + 1);

        // MODIFICATIONS
        size_links_level0_ = maxM0_ * sizeof(tableint) + sizeof(linklistsizeint); 
        size_data_per_element_ = size_links_level0_ + sizeof(labeltype);  
        // old:     size_data_per_element_ = size_links_level0_ + data_size_ + sizeof(labeltype);
        offsetData_ = size_links_level0_;   // this isn't used anymore
        label_offset_ = size_links_level0_; 
        // old:     label_offset_ = size_links_level0_ + data_size_;
        offsetLevel0_ = 0;

        data_level0_memory_ = (char *)malloc(max_elements_ * size_data_per_element_);
        if (data_level0_memory_ == nullptr) throw std::runtime_error("Not enough memory");

        cur_element_count = 0;

        visited_list_pool_ = new VisitedListPool(1, max_elements);

        // initializations for special treatment of the first node
        enterpoint_node_ = -1;
        maxlevel_ = -1;

        linkLists_ = (char **)malloc(sizeof(void *) * max_elements_);
        if (linkLists_ == nullptr)
            throw std::runtime_error("Not enough memory: HierarchicalNSW failed to allocate linklists");
        size_links_per_element_ = maxM_ * sizeof(tableint) + sizeof(linklistsizeint);
        mult_ = 1 / log(1.0 * M_);
        revSize_ = 1.0 / mult_;
    }

    ~HierarchicalNSW() {
        free(data_level0_memory_);
        for (tableint i = 0; i < cur_element_count; i++) {
            if (element_levels_[i] > 0) free(linkLists_[i]);
        }
        free(linkLists_);
        delete visited_list_pool_;
    }

    struct CompareByFirst {
        constexpr bool operator()(std::pair<dist_t, tableint> const &a,
                                  std::pair<dist_t, tableint> const &b) const noexcept {
            return a.first < b.first;
        }
    };

    void setEf(size_t ef) { ef_ = ef; }

    inline std::mutex &getLabelOpMutex(labeltype label) const {
        // calculate hash
        size_t lock_id = label & (MAX_LABEL_OPERATION_LOCKS - 1);
        return label_op_locks_[lock_id];
    }

    inline labeltype getExternalLabel(tableint internal_id) const {
        labeltype return_label;
        memcpy(&return_label, (data_level0_memory_ + internal_id * size_data_per_element_ + label_offset_),
               sizeof(labeltype));
        return return_label;
    }

    inline void setExternalLabel(tableint internal_id, labeltype label) const {
        memcpy((data_level0_memory_ + internal_id * size_data_per_element_ + label_offset_), &label, sizeof(labeltype));
    }

    inline labeltype *getExternalLabeLp(tableint internal_id) const {
        return (labeltype *)(data_level0_memory_ + internal_id * size_data_per_element_ + label_offset_);
    }

    // this gives data representation of the point
    inline char *getDataByInternalId(tableint internal_id) const {
        return (data_pointer_ + internal_id * data_size_); 
        // return (data_level0_memory_ + internal_id * size_data_per_element_ + offsetData_);
    }

    int getRandomLevel(double reverse_size) {
        std::uniform_real_distribution<double> distribution(0.0, 1.0);
        double r = -log(distribution(level_generator_)) * reverse_size;
        return (int)r;
    }

    size_t getMaxElements() { return max_elements_; }

    size_t getCurrentElementCount() { return cur_element_count; }

    size_t getDeletedCount() { return num_deleted_; }

    std::priority_queue<std::pair<dist_t, tableint>, std::vector<std::pair<dist_t, tableint>>, CompareByFirst>
    searchBaseLayer(tableint ep_id, const void *data_point, int layer) {
        VisitedList *vl = visited_list_pool_->getFreeVisitedList();
        vl_type *visited_array = vl->mass;
        vl_type visited_array_tag = vl->curV;

        std::priority_queue<std::pair<dist_t, tableint>, std::vector<std::pair<dist_t, tableint>>, CompareByFirst>
            top_candidates;
        std::priority_queue<std::pair<dist_t, tableint>, std::vector<std::pair<dist_t, tableint>>, CompareByFirst>
            candidateSet;

        dist_t lowerBound;
        // if (!isMarkedDeleted(ep_id)) {
            dist_t dist = fstdistfunc_(data_point, getDataByInternalId(ep_id), dist_func_param_);
            top_candidates.emplace(dist, ep_id);
            lowerBound = dist;
            candidateSet.emplace(-dist, ep_id);
        // } else {
        //     lowerBound = std::numeric_limits<dist_t>::max();
        //     candidateSet.emplace(-lowerBound, ep_id);
        // }
        visited_array[ep_id] = visited_array_tag;

        while (!candidateSet.empty()) {
            std::pair<dist_t, tableint> curr_el_pair = candidateSet.top();
            if ((-curr_el_pair.first) > lowerBound && top_candidates.size() == ef_construction_) {
                break;
            }
            candidateSet.pop();

            tableint curNodeNum = curr_el_pair.second;

            std::unique_lock<std::mutex> lock(link_list_locks_[curNodeNum]);

            int *data;  // = (int *)(linkList0_ + curNodeNum * size_links_per_element0_);
            if (layer == 0) {
                data = (int *)get_linklist0(curNodeNum);
            } else {
                data = (int *)get_linklist(curNodeNum, layer);
                //                    data = (int *) (linkLists_[curNodeNum] + (layer - 1) * size_links_per_element_);
            }
            size_t size = getListCount((linklistsizeint *)data);
            tableint *datal = (tableint *)(data + 1);
#ifdef USE_SSE
            _mm_prefetch((char *)(visited_array + *(data + 1)), _MM_HINT_T0);
            _mm_prefetch((char *)(visited_array + *(data + 1) + 64), _MM_HINT_T0);
            _mm_prefetch(getDataByInternalId(*datal), _MM_HINT_T0);
            _mm_prefetch(getDataByInternalId(*(datal + 1)), _MM_HINT_T0);
#endif

            for (size_t j = 0; j < size; j++) {
                tableint candidate_id = *(datal + j);
//                    if (candidate_id == 0) continue;
#ifdef USE_SSE
                _mm_prefetch((char *)(visited_array + *(datal + j + 1)), _MM_HINT_T0);
                _mm_prefetch(getDataByInternalId(*(datal + j + 1)), _MM_HINT_T0);
#endif
                if (visited_array[candidate_id] == visited_array_tag) continue;
                visited_array[candidate_id] = visited_array_tag;
                char *currObj1 = (getDataByInternalId(candidate_id));

                dist_t dist1 = fstdistfunc_(data_point, currObj1, dist_func_param_);
                if (top_candidates.size() < ef_construction_ || lowerBound > dist1) {
                    candidateSet.emplace(-dist1, candidate_id);
#ifdef USE_SSE
                    _mm_prefetch(getDataByInternalId(candidateSet.top().second), _MM_HINT_T0);
#endif

                    // if (!isMarkedDeleted(candidate_id)) top_candidates.emplace(dist1, candidate_id);
                    top_candidates.emplace(dist1, candidate_id);

                    if (top_candidates.size() > ef_construction_) top_candidates.pop();

                    if (!top_candidates.empty()) lowerBound = top_candidates.top().first;
                }
            }
        }
        visited_list_pool_->releaseVisitedList(vl);

        return top_candidates;
    }


    /**
     * @brief Original Bottom Layer Search- not used
     * 
     * @tparam has_deletions 
     * @tparam collect_metrics 
     * @param ep_id 
     * @param data_point 
     * @param ef 
     * @param isIdAllowed 
     * @return std::priority_queue<std::pair<dist_t, tableint>, std::vector<std::pair<dist_t, tableint>>, CompareByFirst> 
     */
    template <bool has_deletions, bool collect_metrics = false>
    std::priority_queue<std::pair<dist_t, tableint>, std::vector<std::pair<dist_t, tableint>>, CompareByFirst>
    searchBaseLayerST(tableint ep_id, const void *data_point, size_t ef,
                      BaseFilterFunctor *isIdAllowed = nullptr) const {
        VisitedList *vl = visited_list_pool_->getFreeVisitedList();
        vl_type *visited_array = vl->mass;
        vl_type visited_array_tag = vl->curV;

        std::priority_queue<std::pair<dist_t, tableint>, std::vector<std::pair<dist_t, tableint>>, CompareByFirst>
            top_candidates;
        std::priority_queue<std::pair<dist_t, tableint>, std::vector<std::pair<dist_t, tableint>>, CompareByFirst>
            candidate_set;

        dist_t lowerBound;
        if ((!has_deletions || !isMarkedDeleted(ep_id)) &&
            ((!isIdAllowed) || (*isIdAllowed)(getExternalLabel(ep_id)))) {
            dist_t dist = fstdistfunc_(data_point, getDataByInternalId(ep_id), dist_func_param_);
            lowerBound = dist;
            top_candidates.emplace(dist, ep_id);
            candidate_set.emplace(-dist, ep_id);
        } else {
            lowerBound = std::numeric_limits<dist_t>::max();
            candidate_set.emplace(-lowerBound, ep_id);
        }

        visited_array[ep_id] = visited_array_tag;

        while (!candidate_set.empty()) {
            std::pair<dist_t, tableint> current_node_pair = candidate_set.top();

            if ((-current_node_pair.first) > lowerBound &&
                (top_candidates.size() == ef || (!isIdAllowed && !has_deletions))) {
                break;
            }
            candidate_set.pop();

            tableint current_node_id = current_node_pair.second;
            int *data = (int *)get_linklist0(current_node_id);
            size_t size = getListCount((linklistsizeint *)data);
            //                bool cur_node_deleted = isMarkedDeleted(current_node_id);
            if (collect_metrics) {
                metric_hops++;
                metric_distance_computations += size;
            }

#ifdef USE_SSE
            _mm_prefetch((char *)(visited_array + *(data + 1)), _MM_HINT_T0);
            _mm_prefetch((char *)(visited_array + *(data + 1) + 64), _MM_HINT_T0);
            _mm_prefetch(data_level0_memory_ + (*(data + 1)) * size_data_per_element_ + offsetData_, _MM_HINT_T0);
            _mm_prefetch((char *)(data + 2), _MM_HINT_T0);
#endif

            for (size_t j = 1; j <= size; j++) {
                int candidate_id = *(data + j);
//                    if (candidate_id == 0) continue;
#ifdef USE_SSE
                _mm_prefetch((char *)(visited_array + *(data + j + 1)), _MM_HINT_T0);
                _mm_prefetch(data_level0_memory_ + (*(data + j + 1)) * size_data_per_element_ + offsetData_,
                             _MM_HINT_T0);  ////////////
#endif
                if (!(visited_array[candidate_id] == visited_array_tag)) {
                    visited_array[candidate_id] = visited_array_tag;

                    char *currObj1 = (getDataByInternalId(candidate_id));
                    dist_t dist = fstdistfunc_(data_point, currObj1, dist_func_param_);

                    if (top_candidates.size() < ef || lowerBound > dist) {
                        candidate_set.emplace(-dist, candidate_id);
#ifdef USE_SSE
                        _mm_prefetch(data_level0_memory_ + candidate_set.top().second * size_data_per_element_ +
                                         offsetLevel0_,  ///////////
                                     _MM_HINT_T0);       ////////////////////////
#endif

                        if ((!has_deletions || !isMarkedDeleted(candidate_id)) &&
                            ((!isIdAllowed) || (*isIdAllowed)(getExternalLabel(candidate_id))))
                            top_candidates.emplace(dist, candidate_id);

                        if (top_candidates.size() > ef) top_candidates.pop();

                        if (!top_candidates.empty()) lowerBound = top_candidates.top().first;
                    }
                }
            }
        }

        visited_list_pool_->releaseVisitedList(vl);
        return top_candidates;
    }

    // literally the hsp test
    void getNeighborsByHeuristic2(
        std::priority_queue<std::pair<dist_t, tableint>, std::vector<std::pair<dist_t, tableint>>, CompareByFirst>
            &top_candidates,
        const size_t M) {
        if (top_candidates.size() < M) {
            return;
        }

        std::priority_queue<std::pair<dist_t, tableint>> queue_closest;
        std::vector<std::pair<dist_t, tableint>> return_list;
        while (top_candidates.size() > 0) {
            queue_closest.emplace(-top_candidates.top().first, top_candidates.top().second);
            top_candidates.pop();
        }

        while (queue_closest.size()) {
            if (return_list.size() >= M) break;
            std::pair<dist_t, tableint> curent_pair = queue_closest.top();
            dist_t dist_to_query = -curent_pair.first;
            queue_closest.pop();
            bool good = true;

            for (std::pair<dist_t, tableint> second_pair : return_list) {
                dist_t curdist = fstdistfunc_(getDataByInternalId(second_pair.second),
                                              getDataByInternalId(curent_pair.second), dist_func_param_);
                if (curdist < dist_to_query) {
                    good = false;
                    break;
                }
            }
            if (good) {
                return_list.push_back(curent_pair);
            }
        }

        for (std::pair<dist_t, tableint> curent_pair : return_list) {
            top_candidates.emplace(-curent_pair.first, curent_pair.second);
        }
    }

    linklistsizeint *get_linklist0(tableint internal_id) const {
        return (linklistsizeint *)(data_level0_memory_ + internal_id * size_data_per_element_ + offsetLevel0_);
    }

    linklistsizeint *get_linklist0(tableint internal_id, char *data_level0_memory_) const {
        return (linklistsizeint *)(data_level0_memory_ + internal_id * size_data_per_element_ + offsetLevel0_);
    }

    linklistsizeint *get_linklist(tableint internal_id, int level) const {
        return (linklistsizeint *)(linkLists_[internal_id] + (level - 1) * size_links_per_element_);
    }

    linklistsizeint *get_linklist_at_level(tableint internal_id, int level) const {
        return level == 0 ? get_linklist0(internal_id) : get_linklist(internal_id, level);
    }

    tableint mutuallyConnectNewElement(
        const void *data_point, tableint cur_c,
        std::priority_queue<std::pair<dist_t, tableint>, std::vector<std::pair<dist_t, tableint>>, CompareByFirst>
            &top_candidates,
        int level, bool isUpdate) {
        size_t Mcurmax = level ? maxM_ : maxM0_;
        getNeighborsByHeuristic2(top_candidates, M_);
        if (top_candidates.size() > M_)
            throw std::runtime_error("Should be not be more than M_ candidates returned by the heuristic");

        std::vector<tableint> selectedNeighbors;
        selectedNeighbors.reserve(M_);
        while (top_candidates.size() > 0) {
            selectedNeighbors.push_back(top_candidates.top().second);
            top_candidates.pop();
        }

        tableint next_closest_entry_point = selectedNeighbors.back();

        {
            // lock only during the update
            // because during the addition the lock for cur_c is already acquired
            std::unique_lock<std::mutex> lock(link_list_locks_[cur_c], std::defer_lock);
            if (isUpdate) {
                lock.lock();
            }
            linklistsizeint *ll_cur;
            if (level == 0)
                ll_cur = get_linklist0(cur_c);
            else
                ll_cur = get_linklist(cur_c, level);

            if (*ll_cur && !isUpdate) {
                throw std::runtime_error("The newly inserted element should have blank link list");
            }
            setListCount(ll_cur, selectedNeighbors.size());
            tableint *data = (tableint *)(ll_cur + 1);
            for (size_t idx = 0; idx < selectedNeighbors.size(); idx++) {
                if (data[idx] && !isUpdate) throw std::runtime_error("Possible memory corruption");
                if (level > element_levels_[selectedNeighbors[idx]])
                    throw std::runtime_error("Trying to make a link on a non-existent level");

                data[idx] = selectedNeighbors[idx];
            }
        }

        for (size_t idx = 0; idx < selectedNeighbors.size(); idx++) {
            std::unique_lock<std::mutex> lock(link_list_locks_[selectedNeighbors[idx]]);

            linklistsizeint *ll_other;
            if (level == 0)
                ll_other = get_linklist0(selectedNeighbors[idx]);
            else
                ll_other = get_linklist(selectedNeighbors[idx], level);

            size_t sz_link_list_other = getListCount(ll_other);

            if (sz_link_list_other > Mcurmax) throw std::runtime_error("Bad value of sz_link_list_other");
            if (selectedNeighbors[idx] == cur_c) throw std::runtime_error("Trying to connect an element to itself");
            if (level > element_levels_[selectedNeighbors[idx]])
                throw std::runtime_error("Trying to make a link on a non-existent level");

            tableint *data = (tableint *)(ll_other + 1);

            bool is_cur_c_present = false;
            if (isUpdate) {
                for (size_t j = 0; j < sz_link_list_other; j++) {
                    if (data[j] == cur_c) {
                        is_cur_c_present = true;
                        break;
                    }
                }
            }

            // If cur_c is already present in the neighboring connections of `selectedNeighbors[idx]` then no need to
            // modify any connections or run the heuristics.
            if (!is_cur_c_present) {
                if (sz_link_list_other < Mcurmax) {
                    data[sz_link_list_other] = cur_c;
                    setListCount(ll_other, sz_link_list_other + 1);
                } else {
                    // finding the "weakest" element to replace it with the new one
                    dist_t d_max = fstdistfunc_(getDataByInternalId(cur_c), getDataByInternalId(selectedNeighbors[idx]),
                                                dist_func_param_);
                    // Heuristic:
                    std::priority_queue<std::pair<dist_t, tableint>, std::vector<std::pair<dist_t, tableint>>,
                                        CompareByFirst>
                        candidates;
                    candidates.emplace(d_max, cur_c);

                    for (size_t j = 0; j < sz_link_list_other; j++) {
                        candidates.emplace(fstdistfunc_(getDataByInternalId(data[j]),
                                                        getDataByInternalId(selectedNeighbors[idx]), dist_func_param_),
                                           data[j]);
                    }

                    getNeighborsByHeuristic2(candidates, Mcurmax);

                    int indx = 0;
                    while (candidates.size() > 0) {
                        data[indx] = candidates.top().second;
                        candidates.pop();
                        indx++;
                    }

                    setListCount(ll_other, indx);
                }
            }
        }

        return next_closest_entry_point;
    }


    void resizeIndex(size_t new_max_elements) {
        if (new_max_elements < cur_element_count)
            throw std::runtime_error("Cannot resize, max element is less than the current number of elements");

        delete visited_list_pool_;
        visited_list_pool_ = new VisitedListPool(1, new_max_elements);

        element_levels_.resize(new_max_elements);

        std::vector<std::mutex>(new_max_elements).swap(link_list_locks_);

        // Reallocate base layer
        char *data_level0_memory_new = (char *)realloc(data_level0_memory_, new_max_elements * size_data_per_element_);
        if (data_level0_memory_new == nullptr)
            throw std::runtime_error("Not enough memory: resizeIndex failed to allocate base layer");
        data_level0_memory_ = data_level0_memory_new;

        // Reallocate all other layers
        char **linkLists_new = (char **)realloc(linkLists_, sizeof(void *) * new_max_elements);
        if (linkLists_new == nullptr)
            throw std::runtime_error("Not enough memory: resizeIndex failed to allocate other layers");
        linkLists_ = linkLists_new;

        max_elements_ = new_max_elements;
    }

    void saveIndex(const std::string &location) {
        std::ofstream output(location, std::ios::binary);
        std::streampos position;

        writeBinaryPOD(output, offsetLevel0_);
        writeBinaryPOD(output, max_elements_);
        writeBinaryPOD(output, cur_element_count);
        writeBinaryPOD(output, size_data_per_element_);
        writeBinaryPOD(output, label_offset_);
        writeBinaryPOD(output, offsetData_);
        writeBinaryPOD(output, maxlevel_);
        writeBinaryPOD(output, enterpoint_node_);
        writeBinaryPOD(output, maxM_);

        writeBinaryPOD(output, maxM0_);
        writeBinaryPOD(output, M_);
        writeBinaryPOD(output, mult_);
        writeBinaryPOD(output, ef_construction_);

        output.write(data_level0_memory_, cur_element_count * size_data_per_element_);

        for (size_t i = 0; i < cur_element_count; i++) {
            unsigned int linkListSize = element_levels_[i] > 0 ? size_links_per_element_ * element_levels_[i] : 0;
            writeBinaryPOD(output, linkListSize);
            if (linkListSize) output.write(linkLists_[i], linkListSize);
        }
        output.close();
    }

    void loadIndex(const std::string &location, SpaceInterface<dist_t> *s, size_t max_elements_i = 0) {
        std::ifstream input(location, std::ios::binary);

        if (!input.is_open()) throw std::runtime_error("Cannot open file");

        // get file size:
        input.seekg(0, input.end);
        std::streampos total_filesize = input.tellg();
        input.seekg(0, input.beg);

        readBinaryPOD(input, offsetLevel0_);
        readBinaryPOD(input, max_elements_);
        readBinaryPOD(input, cur_element_count);

        size_t max_elements = max_elements_i;
        if (max_elements < cur_element_count) max_elements = max_elements_;
        max_elements_ = max_elements;
        readBinaryPOD(input, size_data_per_element_);
        readBinaryPOD(input, label_offset_);
        readBinaryPOD(input, offsetData_);
        readBinaryPOD(input, maxlevel_);
        readBinaryPOD(input, enterpoint_node_);

        readBinaryPOD(input, maxM_);
        readBinaryPOD(input, maxM0_);
        readBinaryPOD(input, M_);
        readBinaryPOD(input, mult_);
        readBinaryPOD(input, ef_construction_);

        data_size_ = s->get_data_size();
        fstdistfunc_ = s->get_dist_func();
        dist_func_param_ = s->get_dist_func_param();

        auto pos = input.tellg();

        /// Optional - check if index is ok:
        input.seekg(cur_element_count * size_data_per_element_, input.cur);
        for (size_t i = 0; i < cur_element_count; i++) {
            if (input.tellg() < 0 || input.tellg() >= total_filesize) {
                throw std::runtime_error("Index seems to be corrupted or unsupported");
            }

            unsigned int linkListSize;
            readBinaryPOD(input, linkListSize);
            if (linkListSize != 0) {
                input.seekg(linkListSize, input.cur);
            }
        }

        // throw exception if it either corrupted or old index
        if (input.tellg() != total_filesize) throw std::runtime_error("Index seems to be corrupted or unsupported");

        input.clear();
        /// Optional check end

        input.seekg(pos, input.beg);

        data_level0_memory_ = (char *)malloc(max_elements * size_data_per_element_);
        if (data_level0_memory_ == nullptr)
            throw std::runtime_error("Not enough memory: loadIndex failed to allocate level0");
        input.read(data_level0_memory_, cur_element_count * size_data_per_element_);

        size_links_per_element_ = maxM_ * sizeof(tableint) + sizeof(linklistsizeint);

        size_links_level0_ = maxM0_ * sizeof(tableint) + sizeof(linklistsizeint);
        std::vector<std::mutex>(max_elements).swap(link_list_locks_);
        std::vector<std::mutex>(MAX_LABEL_OPERATION_LOCKS).swap(label_op_locks_);

        visited_list_pool_ = new VisitedListPool(1, max_elements);

        linkLists_ = (char **)malloc(sizeof(void *) * max_elements);
        if (linkLists_ == nullptr)
            throw std::runtime_error("Not enough memory: loadIndex failed to allocate linklists");
        element_levels_ = std::vector<int>(max_elements);
        revSize_ = 1.0 / mult_;
        ef_ = 10;
        for (size_t i = 0; i < cur_element_count; i++) {
            label_lookup_[getExternalLabel(i)] = i;
            unsigned int linkListSize;
            readBinaryPOD(input, linkListSize);
            if (linkListSize == 0) {
                element_levels_[i] = 0;
                linkLists_[i] = nullptr;
            } else {
                element_levels_[i] = linkListSize / size_links_per_element_;
                linkLists_[i] = (char *)malloc(linkListSize);
                if (linkLists_[i] == nullptr)
                    throw std::runtime_error("Not enough memory: loadIndex failed to allocate linklist");
                input.read(linkLists_[i], linkListSize);
            }
        }

        for (size_t i = 0; i < cur_element_count; i++) {
            if (isMarkedDeleted(i)) {
                num_deleted_ += 1;
                if (allow_replace_deleted_) deleted_elements.insert(i);
            }
        }

        input.close();

        return;
    }

    template <typename data_t>
    std::vector<data_t> getDataByLabel(labeltype label) const {
        // lock all operations with element by label
        std::unique_lock<std::mutex> lock_label(getLabelOpMutex(label));

        std::unique_lock<std::mutex> lock_table(label_lookup_lock);
        auto search = label_lookup_.find(label);
        // if (search == label_lookup_.end() || isMarkedDeleted(search->second)) {
        if (search == label_lookup_.end()) {
            throw std::runtime_error("Label not found");
        }
        tableint internalId = search->second;
        lock_table.unlock();

        char *data_ptrv = getDataByInternalId(internalId);
        size_t dim = *((size_t *)dist_func_param_);
        std::vector<data_t> data;
        data_t *data_ptr = (data_t *)data_ptrv;
        for (int i = 0; i < (int) dim; i++) {
            data.push_back(*data_ptr);
            data_ptr += 1;
        }
        return data;
    }

    /**
     * ===========================================================
     * 
     *          KEEPING DELETED FUNCTIONALITY, BUT NOT USED
     * 
     * ===========================================================
    */

    /*
     * Marks an element with the given label deleted, does NOT really change the current graph.
     */
    void markDelete(labeltype label) {
        // lock all operations with element by label
        std::unique_lock<std::mutex> lock_label(getLabelOpMutex(label));

        std::unique_lock<std::mutex> lock_table(label_lookup_lock);
        auto search = label_lookup_.find(label);
        if (search == label_lookup_.end()) {
            throw std::runtime_error("Label not found");
        }
        tableint internalId = search->second;
        lock_table.unlock();

        markDeletedInternal(internalId);
    }

    /*
     * Uses the last 16 bits of the memory for the linked list size to store the mark,
     * whereas maxM0_ has to be limited to the lower 16 bits, however, still large enough in almost all cases.
     */
    void markDeletedInternal(tableint internalId) {
        assert(internalId < cur_element_count);
        if (!isMarkedDeleted(internalId)) {
            unsigned char *ll_cur = ((unsigned char *)get_linklist0(internalId)) + 2;
            *ll_cur |= DELETE_MARK;
            num_deleted_ += 1;
            if (allow_replace_deleted_) {
                std::unique_lock<std::mutex> lock_deleted_elements(deleted_elements_lock);
                deleted_elements.insert(internalId);
            }
        } else {
            throw std::runtime_error("The requested to delete element is already deleted");
        }
    }

    /*
     * Removes the deleted mark of the node, does NOT really change the current graph.
     *
     * Note: the method is not safe to use when replacement of deleted elements is enabled,
     *  because elements marked as deleted can be completely removed by addPoint
     */
    void unmarkDelete(labeltype label) {
        // lock all operations with element by label
        std::unique_lock<std::mutex> lock_label(getLabelOpMutex(label));

        std::unique_lock<std::mutex> lock_table(label_lookup_lock);
        auto search = label_lookup_.find(label);
        if (search == label_lookup_.end()) {
            throw std::runtime_error("Label not found");
        }
        tableint internalId = search->second;
        lock_table.unlock();

        unmarkDeletedInternal(internalId);
    }

    /*
     * Remove the deleted mark of the node.
     */
    void unmarkDeletedInternal(tableint internalId) {
        assert(internalId < cur_element_count);
        if (isMarkedDeleted(internalId)) {
            unsigned char *ll_cur = ((unsigned char *)get_linklist0(internalId)) + 2;
            *ll_cur &= ~DELETE_MARK;
            num_deleted_ -= 1;
            if (allow_replace_deleted_) {
                std::unique_lock<std::mutex> lock_deleted_elements(deleted_elements_lock);
                deleted_elements.erase(internalId);
            }
        } else {
            throw std::runtime_error("The requested to undelete element is not deleted");
        }
    }

    /*
     * Checks the first 16 bits of the memory to see if the element is marked deleted.
     */
    bool isMarkedDeleted(tableint internalId) const {
        unsigned char *ll_cur = ((unsigned char *)get_linklist0(internalId)) + 2;
        return *ll_cur & DELETE_MARK;
    }

    unsigned short int getListCount(linklistsizeint *ptr) const { return *((unsigned short int *)ptr); }

    void setListCount(linklistsizeint *ptr, unsigned short int size) const {
        *((unsigned short int *)(ptr)) = *((unsigned short int *)&size);
    }

    /**
     * ======================================================
     * 
     *          REMOVED FUNCTIONS FOR UPDATES 
     * 
     * ======================================================
     */

    /*
     * Adds point. Updates the point if it is already in the index.
     * If replacement of deleted elements is enabled: replaces previously deleted point if any, updating it with new
     * point
     */
    void addPoint(const void *data_point, labeltype label, bool replace_deleted = false) {
        if ((allow_replace_deleted_ == false) && (replace_deleted == true)) {
            throw std::runtime_error("Replacement of deleted elements is disabled in constructor");
        }
        addPoint(data_point, label, -1);
    }

    /**
     * @brief Add Point To The Level
     * 
     * @param data_point 
     * @param label 
     * @param level 
     * @return tableint 
     */
    tableint addPoint(const void *data_point, labeltype label, int level) {
        tableint cur_c = 0;
        {
            // Checking if the element with the same label already exists
            // if so, updating it *instead* of creating a new element.
            std::unique_lock<std::mutex> lock_table(label_lookup_lock);

            // EDIT: REMOVED CHECK FOR DELETED FUNCTIONALITY
            if (cur_element_count >= max_elements_) {
                throw std::runtime_error("The number of elements exceeds the specified limit");
            }

            cur_c = cur_element_count;
            cur_element_count++;
            label_lookup_[label] = cur_c;
        }

        std::unique_lock<std::mutex> lock_el(link_list_locks_[cur_c]);
        int curlevel = getRandomLevel(mult_);
        if (level > 0) curlevel = level;
        element_levels_[cur_c] = curlevel;

        std::unique_lock<std::mutex> templock(global);
        int maxlevelcopy = maxlevel_;
        if (curlevel <= maxlevelcopy) templock.unlock();
        tableint currObj = enterpoint_node_;

        // set the memory for this element
        memset(data_level0_memory_ + cur_c * size_data_per_element_ + offsetLevel0_, 0, size_data_per_element_);

        // Initialisation of the data and label
        memcpy(getExternalLabeLp(cur_c), &label, sizeof(labeltype));
        // memcpy(getDataByInternalId(cur_c), data_point, data_size_); // REMOVED: not storing memory anymore

        if (curlevel) {
            linkLists_[cur_c] = (char *)malloc(size_links_per_element_ * curlevel + 1);
            if (linkLists_[cur_c] == nullptr)
                throw std::runtime_error("Not enough memory: addPoint failed to allocate linklist");
            memset(linkLists_[cur_c], 0, size_links_per_element_ * curlevel + 1);
        }

        if ((signed)currObj != -1) {
            if (curlevel < maxlevelcopy) {
                dist_t curdist = fstdistfunc_(data_point, getDataByInternalId(currObj), dist_func_param_);
                for (int level = maxlevelcopy; level > curlevel; level--) {
                    bool changed = true;
                    while (changed) {
                        changed = false;
                        unsigned int *data;
                        std::unique_lock<std::mutex> lock(link_list_locks_[currObj]);
                        data = get_linklist(currObj, level);
                        int size = getListCount(data);

                        tableint *datal = (tableint *)(data + 1);
                        for (int i = 0; i < size; i++) {
                            tableint cand = datal[i];
                            if (cand < 0 || cand > max_elements_) throw std::runtime_error("cand error");
                            dist_t d = fstdistfunc_(data_point, getDataByInternalId(cand), dist_func_param_);
                            if (d < curdist) {
                                curdist = d;
                                currObj = cand;
                                changed = true;
                            }
                        }
                    }
                }
            }

            for (int level = std::min(curlevel, maxlevelcopy); level >= 0; level--) {
                if (level > maxlevelcopy || level < 0)  // possible?
                    throw std::runtime_error("Level error");

                std::priority_queue<std::pair<dist_t, tableint>, std::vector<std::pair<dist_t, tableint>>,
                                    CompareByFirst>
                    top_candidates = searchBaseLayer(currObj, data_point, level);
                currObj = mutuallyConnectNewElement(data_point, cur_c, top_candidates, level, false);
            }
        } else {
            // Do nothing for the first element
            enterpoint_node_ = 0;
            maxlevel_ = curlevel;
        }

        // Releasing lock for the maximum level
        if (curlevel > maxlevelcopy) {
            enterpoint_node_ = cur_c;
            maxlevel_ = curlevel;
        }
        return cur_c;
    }


    void checkIntegrity() {
        int connections_checked = 0;
        std::vector<int> inbound_connections_num(cur_element_count, 0);
        for (int i = 0; i < cur_element_count; i++) {
            for (int l = 0; l <= element_levels_[i]; l++) {
                linklistsizeint *ll_cur = get_linklist_at_level(i, l);
                int size = getListCount(ll_cur);
                tableint *data = (tableint *)(ll_cur + 1);
                std::unordered_set<tableint> s;
                for (int j = 0; j < size; j++) {
                    assert(data[j] > 0);
                    assert(data[j] < cur_element_count);
                    assert(data[j] != i);
                    inbound_connections_num[data[j]]++;
                    s.insert(data[j]);
                    connections_checked++;
                }
                assert(s.size() == size);
            }
        }
        if (cur_element_count > 1) {
            int min1 = inbound_connections_num[0], max1 = inbound_connections_num[0];
            for (int i = 0; i < cur_element_count; i++) {
                assert(inbound_connections_num[i] > 0);
                min1 = std::min(inbound_connections_num[i], min1);
                max1 = std::max(inbound_connections_num[i], max1);
            }
            std::cout << "Min inbound: " << min1 << ", Max inbound:" << max1 << "\n";
        }
        std::cout << "integrity ok, checked " << connections_checked << " connections\n";
    }

        /**
     * @brief ORIGINAL HNSW SEARCH FUNCTION
     * 
     * @param query_data 
     * @param k 
     * @param isIdAllowed 
     * @return std::priority_queue<std::pair<dist_t, labeltype>> 
     */
    std::priority_queue<std::pair<dist_t, labeltype>> searchKnn(const void *query_data, size_t k,
                                                                BaseFilterFunctor *isIdAllowed = nullptr) const {
        std::priority_queue<std::pair<dist_t, labeltype>> result;
        if (cur_element_count == 0) return result;

        tableint currObj = enterpoint_node_;
        dist_t curdist = fstdistfunc_(query_data, getDataByInternalId(enterpoint_node_), dist_func_param_);

        for (int level = maxlevel_; level > 0; level--) {
            bool changed = true;
            while (changed) {
                changed = false;
                unsigned int *data;

                data = (unsigned int *)get_linklist(currObj, level);
                int size = getListCount(data);
                metric_hops++;
                metric_distance_computations += size;
                // printf(" Level %d, Point %u, NN: %d\n", level, currObj, size);

                tableint *datal = (tableint *)(data + 1);
                for (int i = 0; i < size; i++) {
                    tableint cand = datal[i];
                    // printf("  *  %u\n", cand);

                    if (cand < 0 || cand > max_elements_) {
                        printf(" Level %d, Point %u, cand: %u\n", level, currObj, (unsigned int)cand);
                        throw std::runtime_error("cand error");
                    }
                    dist_t d = fstdistfunc_(query_data, getDataByInternalId(cand), dist_func_param_);

                    if (d < curdist) {
                        curdist = d;
                        currObj = cand;
                        changed = true;
                    }
                }
            }
        }

        std::priority_queue<std::pair<dist_t, tableint>, std::vector<std::pair<dist_t, tableint>>, CompareByFirst>
            top_candidates;
        if (num_deleted_) {
            top_candidates = searchBaseLayerST<true, true>(currObj, query_data, std::max(ef_, k), isIdAllowed);
        } else {
            top_candidates = searchBaseLayerST<false, true>(currObj, query_data, std::max(ef_, k), isIdAllowed);
        }

        while (top_candidates.size() > k) {
            top_candidates.pop();
        }
        while (top_candidates.size() > 0) {
            std::pair<dist_t, tableint> rez = top_candidates.top();
            result.push(std::pair<dist_t, labeltype>(rez.first, getExternalLabel(rez.second)));
            top_candidates.pop();
        }
        return result;
    }


    /**
     * ================================================
     * 
     *              BEGIN NEW FUNCTIONS
     * 
     * ================================================
     */


    // Get Points in Each Level
    void getPoints(int level, std::vector<unsigned int> &pointsList) {
        if (level == 0) {
            printf("Level: %d is just the bottom, all points\n", level);
            // return;
        }

        if (level > maxlevel_) {
            printf("Max Level is: %d\n", maxlevel_);
            return;
        }

        // get the points in the layer
        pointsList.clear();
        for (unsigned int i = 0; i < cur_element_count; i++) {
            if (element_levels_[i] >= level) {
                pointsList.push_back(i);
            }
        }

        return;
    }

    // Print Number of Points on each level
    void printHierarchy() {
        printf("Printing the number of points on each level:\n");
        for (int i = maxlevel_; i >= 0; i--) {
            std::vector<unsigned int> pointsInLevel{};
            getPoints(i, pointsInLevel);
            printf("  L-%d: %u\n", i, (unsigned int)pointsInLevel.size());
        }
    }

    // find the hsp neighbors of queryIndex within list L
    void HSP_Test(labeltype queryIndex, std::vector<labeltype> pivots_list, int k, std::vector<labeltype> &neighbors) {
        neighbors.clear();
        char *q_data = getDataByInternalId(queryIndex);

        // only perform on k closest elements
        std::vector<std::pair<dist_t, labeltype>> L{};

        // find next nearest neighbor and create list of distances
        labeltype index1;
        dist_t distance_Q1 = HUGE_VAL;
        for (int it1 = 0; it1 < (int)pivots_list.size(); it1++) {
            labeltype const index = pivots_list[it1];
            if (index == queryIndex) continue;
            dist_t const d = fstdistfunc_(q_data, getDataByInternalId(index), dist_func_param_);
            if (d < distance_Q1) {
                distance_Q1 = d;
                index1 = index;
            }
            L.emplace_back(d, index);
        }

        // only want to perform hsp algo on top k neighbors
        if ((k > 0) && (k < (int)pivots_list.size())) {
            typename std::vector<std::pair<dist_t, labeltype>>::iterator position_to_sort = L.begin() + k;
            std::nth_element(L.begin(), position_to_sort, L.end());
            while (L.size() > (size_t)k) L.pop_back();
        }

        // now, eliminate points and find next hsp neighbors
        while (L.size() > 0) {
            // adding the new neighbor
            neighbors.push_back(index1);

            // prepare for elimination, and for finding next neighbor
            char *index1_data = getDataByInternalId(index1);
            std::vector<std::pair<dist_t, labeltype>> L_copy = L;
            L.clear();
            labeltype index1_next;
            dist_t distance_Q1_next = HUGE_VAL;

            // compute distances in parallel
            for (int it1 = 0; it1 < (int)L_copy.size(); it1++) {
                labeltype const index2 = L_copy[it1].second;
                if (index2 == index1 || index2 == queryIndex) continue;
                dist_t const distance_Q2 = L_copy[it1].first;
                dist_t const distance_12 = fstdistfunc_(index1_data, getDataByInternalId(index2), dist_func_param_);

                // check inequalities
                if (distance_Q1 >= distance_Q2 || distance_12 >= distance_Q2) {
                    L.emplace_back(distance_Q2, index2);
                    if (distance_Q2 < distance_Q1_next) {
                        distance_Q1_next = distance_Q2;
                        index1_next = index2;
                    }
                }
            }

            // setup the next hsp neighbor
            index1 = index1_next;
            distance_Q1 = distance_Q1_next;
        }

        return;
    }

    // delete pointers for all pivots and all its links
    void deletePivotsAndLinks() {
        for (tableint i = 0; i < cur_element_count; i++) {
            if (element_levels_[i] > 0) free(linkLists_[i]);
        }
        // free(linkLists_);
    }

    /**
     * @brief Select Top Layer Pivots and Assign All Elements to the Closest Pivot Domains
     *
     * @param radius        | radius of the top layer
     * @param numThreads    | number of threads to use
     * @param pivots        | output: list of top layer pivots
     * @param assignments   | output: closest pivot for each element
     */
    void topLayerPivotSelection(dist_t radius, int numThreads, std::vector<labeltype> &pivots,
                                std::vector<labeltype> &assignments) {
        pivots.clear();
        assignments.resize(cur_element_count);
        std::vector<bool> vec_covered(cur_element_count, false);  // bool: if element is covered yet

        //> Select the Top Layer Pivots: Batch Construction Approach
        for (labeltype p = 0; p < cur_element_count; p++) {
            if (vec_covered[p] == true) continue;
            pivots.push_back(p);
            vec_covered[p] = true;
            char *pivot_data = getDataByInternalId(p);

// - iterate through all uncovered points, test if they are covered by this new pivot
#pragma omp parallel for schedule(static) num_threads(numThreads)
            for (labeltype x = p + 1; x < cur_element_count; x++) {
                if (vec_covered[x] == true) continue;
                dist_t distance = fstdistfunc_(pivot_data, getDataByInternalId(x), dist_func_param_);
                if (distance <= radius) {
                    vec_covered[x] = true;
                }
            }
        }

//> Assign Each Point To Its Closest Pivot
#pragma omp parallel for schedule(static) num_threads(numThreads)
        for (labeltype x = 0; x < cur_element_count; x++) {
            char *element_data = getDataByInternalId(x);
            labeltype closestPivot;
            dist_t closestPivotDistance = 10000;  // Large number

            // - iterate through all pivots, find closest
            for (int itp = 0; itp < (int)pivots.size(); itp++) {
                labeltype pivot_index = pivots[itp];
                dist_t distance = fstdistfunc_(element_data, getDataByInternalId(pivot_index), dist_func_param_);

                // - one pivot definitely covers, so no need to check
                if (distance < closestPivotDistance) {
                    closestPivotDistance = distance;
                    closestPivot = pivot_index;
                }
            }

            assignments[x] = closestPivot;
        }

        return;
    }

    /**
     * @brief
     *
     * @param radius
     * @param threads
     * @param partition
     * @param pivots
     */
    void secondLayerPivotSelection(dist_t radius, std::vector<labeltype> const &partition, int numThreads,
                                   std::vector<labeltype> &pivots) {
        // pivots.clear();
        int numElements = (int)partition.size();
        std::vector<bool> vec_covered(numElements, false);  // bool: if element is covered yet

        //> First given pivot tested for coverage
        if (pivots.size() >= 1) {
            labeltype pivot_index1 = pivots[0];
            char *pivot1_data = getDataByInternalId(pivot_index1);

            #pragma omp parallel for schedule(static) num_threads(numThreads)
            for (int itx = 0; itx < numElements; itx++) {
                if (partition[itx] == pivot_index1) {
                    vec_covered[itx] = true;
                    continue;
                }
                labeltype element_index = partition[itx];
                dist_t distance = fstdistfunc_(pivot1_data, getDataByInternalId(element_index), dist_func_param_);
                if (distance <= radius) {
                    vec_covered[itx] = true;
                }
            }
        }

        //> Select the Second Layer Pivots From Partition: Batch Construction Approach
        for (int itp = 0; itp < numElements; itp++) {
            if (vec_covered[itp] == true) continue;
            vec_covered[itp] = true;
            labeltype pivot_index = partition[itp];
            pivots.push_back(pivot_index);
            char *pivot_data = getDataByInternalId(pivot_index);

            // - iterate through all uncovered points, test if they are covered by this new pivot
            #pragma omp parallel for schedule(static) num_threads(numThreads)
            for (int itx = itp + 1; itx < numElements; itx++) {
                if (vec_covered[itx] == true) continue;
                labeltype element_index = partition[itx];
                dist_t distance = fstdistfunc_(pivot_data, getDataByInternalId(element_index), dist_func_param_);
                if (distance <= radius) {
                    vec_covered[itx] = true;
                }
            }
        }
        return;
    }



    /**
     * @brief Create the New Hierarchy for HNSW
     * 
     * @param radius1 
     * @param radius2 
     * @param max_neighborhood_size 
     * @param max_neighbors 
     */
    void selectPivotsAndComputeHSP(dist_t radius1, dist_t radius2, int const max_neighborhood_size, int const max_neighbors) {
        int numThreads = (int)omp_get_max_threads();
        printf("NumThreads: %d\n", numThreads);

        //=======================================================
        //------| PART ONE: PIVOT SELECTION
        //=======================================================
        printf("Pivot Selection:\n");

        //> Step One: Top Layer Pivot Selection
        std::vector<labeltype> top_layer_pivots{};
        std::vector<labeltype> element_assignments{};
        topLayerPivotSelection(radius1, numThreads, top_layer_pivots, element_assignments);
        int const number_of_top_layer_pivots = (int) top_layer_pivots.size();
        printf("  * Layer 1: r1=%.4f, |P1|=%d\n", radius1, number_of_top_layer_pivots);

        // - Create a reverse-map to identify top-layer pivots
        std::unordered_map<labeltype, int> top_layer_map{};
        for (int itp = 0; itp < number_of_top_layer_pivots; itp++) {
            labeltype pivot_index = top_layer_pivots[itp];
            top_layer_map.emplace(pivot_index, itp);
        }
        printf("  * Initialized Layer 1 Index->ID Mapping\n");


        // - Organize All Elements Into Top Layer Pivot Domains
        std::vector<std::vector<labeltype>> first_layer_pivot_domains{};
        first_layer_pivot_domains.resize(number_of_top_layer_pivots);
        #pragma omp parallel for schedule(static) num_threads(numThreads)
        for (int itp = 0; itp < number_of_top_layer_pivots; itp++) {
            labeltype pivot_index = top_layer_pivots[itp];
            for (labeltype x = 0; x < cur_element_count; x++) {
                if (element_assignments[x] == pivot_index) {
                    first_layer_pivot_domains[itp].push_back(x);
                }
            }
        }
        printf("  * Collected Layer 1 Partitions\n");

        //> Step Two: Second Layer Pivot Selection
        std::vector<labeltype> second_layer_pivots{};
        for (int itp = 0; itp < number_of_top_layer_pivots; itp++) {
            labeltype pivot_index = top_layer_pivots[itp];
            std::vector<labeltype>& pivot_partition = first_layer_pivot_domains[itp];

            // perform the pivot selection
            std::vector<labeltype> temp_second_layer_pivots = {pivot_index};
            secondLayerPivotSelection(radius2, pivot_partition, numThreads, temp_second_layer_pivots);
            second_layer_pivots.insert(second_layer_pivots.begin(), temp_second_layer_pivots.begin(),
                                       temp_second_layer_pivots.end());
        }
        int const number_of_second_layer_pivots = (int) second_layer_pivots.size();
        printf("  * Layer 2: r2=%.4f, |P2|=%d\n", radius2, number_of_second_layer_pivots);

        // - Create a reverse-map to identify second-layer pivots
        std::unordered_map<labeltype, int> second_layer_map{};
        for (int itp = 0; itp < number_of_second_layer_pivots; itp++) {
            labeltype pivot_index = second_layer_pivots[itp];
            second_layer_map.emplace(pivot_index, itp);
        }
        printf("  * Initialized Layer 2 Index->ID Mapping\n");

        //> - Get pivot domains of layer 2 pivots in top layer
        first_layer_pivot_domains.clear();
        first_layer_pivot_domains.resize(number_of_top_layer_pivots);
        // #pragma omp parallel for schedule(static) num_threads(numThreads)
        for (int itp = 0; itp < number_of_top_layer_pivots; itp++) {
            labeltype pivot_index = top_layer_pivots[itp];

            // - find all second layer pivots that belong to the domain
            for (int itx = 0; itx < number_of_second_layer_pivots; itx++) {
                labeltype element_index = second_layer_pivots[itx];
                if (element_assignments[element_index] == pivot_index) {
                    first_layer_pivot_domains[itp].push_back(element_index);
                }
            }
        }
        printf("  * Re-Collected Layer 1 For Layer 2 Pivots\n");

        //=======================================================
        //------| PART TWO: HSP GRAPH CONSTRUCTION
        //=======================================================
        printf("HSP GRAPH CONSTRUCTION:\n");

        // Step Three: Compute Top Layer HSP
        printf("  * Begin Layer 1 HSP Graph\n");
        std::vector<std::vector<labeltype>> top_layer_hsp_graph{};
        top_layer_hsp_graph.resize(number_of_top_layer_pivots);
        #pragma omp parallel for schedule(static) num_threads(numThreads)
        for (int itp = 0; itp < number_of_top_layer_pivots; itp++) {
            labeltype pivot_index = top_layer_pivots[itp];
            HSP_Test(pivot_index, top_layer_pivots, max_neighborhood_size, top_layer_hsp_graph[itp]);
        }
        printf("  |--Stats on Top Layer HSP: \n");
        int minDegree1 = number_of_top_layer_pivots + 1;
        int maxDegree1 = 0;
        double aveDegree1 = 0;
        for (int itp = 0; itp < number_of_top_layer_pivots; itp++) {
            int numNeighbors = (int)top_layer_hsp_graph[itp].size();
            if (numNeighbors < minDegree1) minDegree1 = numNeighbors;
            if (numNeighbors > maxDegree1) maxDegree1 = numNeighbors;
            aveDegree1 += (double)numNeighbors;
        }
        printf("    - min degree1: %d\n", minDegree1);
        printf("    - max degree1: %d\n", maxDegree1);
        printf("    - ave degree1: %.4f\n", aveDegree1 / (double)number_of_top_layer_pivots);

        printf("  * Begin Layer 2 HSP Graph\n");
        double ave_neighborhood_size = 0;
        // Step Four: Compute Second Layer HSP
        std::vector<std::vector<labeltype>> second_layer_hsp_graph{};
        second_layer_hsp_graph.resize(number_of_second_layer_pivots);
        for (int itp = 0; itp < number_of_top_layer_pivots; itp++) {
            labeltype pivot_index = top_layer_pivots[itp];
            std::vector<labeltype> const &pivot_partition = first_layer_pivot_domains[itp];
            std::vector<labeltype> const &hsp_neighbors = top_layer_hsp_graph[itp];

            // - collect hsp domain of pivot_index
            std::vector<labeltype> hsp_domain = pivot_partition;
            for (int itn = 0; itn < (int)hsp_neighbors.size(); itn++) {
                labeltype neighbor_index = hsp_neighbors[itn];
                int top_pivot_iterator = top_layer_map[neighbor_index];
                std::vector<labeltype> const &neighbor_partition = first_layer_pivot_domains[top_pivot_iterator];
                hsp_domain.insert(hsp_domain.begin(), neighbor_partition.begin(), neighbor_partition.end());
            }
            ave_neighborhood_size += (double) hsp_domain.size();

            // - find hsp neighbors for all pivots in domain in parallel from same spotlight
            #pragma omp parallel for schedule(static) num_threads(numThreads)
            for (int itx = 0; itx < (int)pivot_partition.size(); itx++) {
                labeltype const query_index = pivot_partition[itx];
                int second_pivot_iterator = second_layer_map[query_index];
                if (second_pivot_iterator < 0 || second_pivot_iterator >= number_of_second_layer_pivots) {
                    throw std::runtime_error("second-layer iterator error: q map not within pivots");
                }
                HSP_Test(query_index, hsp_domain, max_neighborhood_size, second_layer_hsp_graph[second_pivot_iterator]);
            }
        }

        printf("|-- Stats on Second Layer HSP: \n");
        int minDegree2 = number_of_second_layer_pivots + 1;
        int maxDegree2 = 0;
        double aveDegree2 = 0;
        for (int itp = 0; itp < number_of_second_layer_pivots; itp++) {
            int numNeighbors = (int)second_layer_hsp_graph[itp].size();
            if (numNeighbors < minDegree2) minDegree2 = numNeighbors;
            if (numNeighbors > maxDegree2) maxDegree2 = numNeighbors;
            aveDegree2 += (double)numNeighbors;
        }
        printf("    - ave hsp domain size: %.4f\n", ave_neighborhood_size / (double) number_of_top_layer_pivots);
        printf("    - max neighborhood size: %d\n", max_neighborhood_size);
        printf("    - min degree2: %d\n", minDegree2);
        printf("    - max degree2: %d\n", maxDegree2);
        printf("    - ave degree2: %.4f\n", aveDegree2 / (double)number_of_second_layer_pivots);

        //=======================================================
        //------| PART THREE: SETTING HIERARCHY
        //=======================================================
        printf("SETTING HIERARCHY:\n");

        //  A: Delete the existing pivots and their links, change all pivots to bottom layer
        deletePivotsAndLinks();
        for (size_t element = 0; element < (size_t)cur_element_count; element++) {
            element_levels_[element] = 0;
        }
        printf("  -> Deleted Links\n");

        //  B. Update the new max level
        maxlevel_ = 2;
        printf("  -> Set Max Levels: %d\n", maxlevel_);

        //  C. Add Pivots To Second Layer
#pragma omp parallel for schedule(static) num_threads(numThreads)
        for (int itp = 0; itp < number_of_second_layer_pivots; itp++) {
            tableint const pivot_index = second_layer_pivots[itp];
            element_levels_[pivot_index] = 1;  // second layer
        }

        //  D. Add Pivots To Top Layer
#pragma omp parallel for schedule(static) num_threads(numThreads)
        for (int itp = 0; itp < number_of_top_layer_pivots; itp++) {
            tableint const pivot_index = top_layer_pivots[itp];
            element_levels_[pivot_index] = 2;  // second layer
        }
        enterpoint_node_ = top_layer_pivots[0];
        printf("  -> Added All Pivots\n");

        // E. Allocate the memory needed for each pivot (MODIFYING)
        maxM_ = (size_t) max_neighbors; // MAX NUMBER OF TOP LAYER PIVOTS
        size_links_per_element_ = maxM_ * sizeof(tableint) + sizeof(linklistsizeint); // original, line~124
        for (tableint pivot_index = 0; pivot_index < (tableint)cur_element_count; pivot_index++) {
            if (element_levels_[pivot_index] == 0) continue;
            linkLists_[pivot_index] = (char *)malloc(size_links_per_element_ * element_levels_[pivot_index] + 1);
        }
        printf("  -> Initialized memory\n");

        // F. Set the Neighbors of the Top Level Pivots
        #pragma omp parallel for schedule(static) num_threads(numThreads)
        for (int itp = 0; itp < (int)top_layer_pivots.size(); itp++) {
            tableint const pivot_index = top_layer_pivots[itp];
            std::vector<labeltype> neighbors = top_layer_hsp_graph[itp];

            // can only have up to max_neighbors;
            while (max_neighbors < (int)neighbors.size()) {
                neighbors.pop_back();
            }

            // get pointer to the list of neighbors.
            linklistsizeint *ll_cur = get_linklist(pivot_index, 2);  // top level is 2

            // initialize this pointer with however many neighbors there actually are
            setListCount(ll_cur, (unsigned short)neighbors.size());

            // format the pointer of this
            tableint *data = (tableint *)(ll_cur + 1);

            // add the neighbors!
            for (int it2 = 0; it2 < (int)neighbors.size(); it2++) {
                data[it2] = (tableint) neighbors[it2];
            }
        }

        // F. Set the Neighbors of the Second Level Pivots
        #pragma omp parallel for schedule(static) num_threads(numThreads)
        for (int itp = 0; itp < (int)second_layer_pivots.size(); itp++) {
            tableint const pivot_index = second_layer_pivots[itp];
            std::vector<labeltype> neighbors = second_layer_hsp_graph[itp];

            // can only have up to max_neighbors;
            while (max_neighbors < (int)neighbors.size()) {
                neighbors.pop_back();
            }

            // get pointer to the list of neighbors.
            linklistsizeint *ll_cur = get_linklist(pivot_index, 1);  // middle level is 1

            // initialize this pointer with however many neighbors there actually are
            setListCount(ll_cur, (unsigned short)neighbors.size());

            // format the pointer of this
            tableint *data = (tableint *)(ll_cur + 1);

            // add the neighbors!
            for (int it2 = 0; it2 < (int)neighbors.size(); it2++) {
                data[it2] = (tableint)neighbors[it2];
            }
        }
        printf("  -> Added Links\n");

        return;
    }





    /**
     * ============================================================
     * 
     *      -------------------------------------------------
     *      |                                               |
     *      |               SEARCH ALGORITHMS               |
     *      |                                               |
     *      -------------------------------------------------
     * 
     * ============================================================
     */


    /**
     * 
     * @brief Revised HNSW Search Algorithm: contained, no filter
     * 
     * @param query_data 
     * @param k 
     * @return std::priority_queue<std::pair<dist_t, labeltype>> 
     */
    std::priority_queue<std::pair<dist_t, labeltype>> search_hnsw(const void *query_data, size_t k) const {
        std::priority_queue<std::pair<dist_t, labeltype>> result;
        if (cur_element_count == 0) return result;
        bool collect_metrics = true;

        //> Default Entry Point
        tableint currObj = enterpoint_node_;
        dist_t curdist = fstdistfunc_(query_data, getDataByInternalId(enterpoint_node_), dist_func_param_);

        //> Top-Down Greedy Search for Entry-Point
        for (int level = maxlevel_; level > 0; level--) {
            bool changed = true;
            while (changed) {
                changed = false;
                unsigned int *data;

                data = (unsigned int *)get_linklist(currObj, level);
                int size = getListCount(data);
                metric_hops++;
                metric_distance_computations += size;

                tableint *datal = (tableint *)(data + 1);
                for (int i = 0; i < size; i++) {
                    tableint cand = datal[i];

                    if (cand < 0 || cand > max_elements_) {
                        throw std::runtime_error("cand error");
                    }
                    dist_t d = fstdistfunc_(query_data, getDataByInternalId(cand), dist_func_param_);

                    if (d < curdist) {
                        curdist = d;
                        currObj = cand;
                        changed = true;
                    }
                }
            }
        }

        //> SEARCH THE BOTTOM LAYER
        size_t ef = std::max(ef_, k);

        // - initialize visited list for tabu search
        VisitedList *vl = visited_list_pool_->getFreeVisitedList();
        vl_type *visited_array = vl->mass;
        vl_type visited_array_tag = vl->curV;

        // - initialize lists
        std::priority_queue<std::pair<dist_t, tableint>, std::vector<std::pair<dist_t, tableint>>, CompareByFirst>
            top_candidates;     // containing the closest visited nodes (to be size ef)
        std::priority_queue<std::pair<dist_t, tableint>, std::vector<std::pair<dist_t, tableint>>, CompareByFirst>
            candidate_set;      // containing the list of nodes to visit

        // - initialize with bottom layer graph entry point
        dist_t dist = fstdistfunc_(query_data, getDataByInternalId(currObj), dist_func_param_);
        dist_t lowerBound = dist;
        top_candidates.emplace(dist, currObj);
        candidate_set.emplace(-dist, currObj);
        visited_array[currObj] = visited_array_tag;

        // - depth-first iteratation through the list of candidate points on bottom layer
        while (!candidate_set.empty()) {
            std::pair<dist_t, tableint> current_node_pair = candidate_set.top();

            // termination condition: no candidate points in top_candidates, top_candidates is full
            if ((-current_node_pair.first) > lowerBound && (top_candidates.size() == ef)) {
                break;
            }
            candidate_set.pop();

            // gather neighbors of current node
            tableint current_node_id = current_node_pair.second;
            int *data = (int *)get_linklist0(current_node_id);
            size_t size = getListCount((linklistsizeint *)data);
            if (collect_metrics) {
                metric_hops++;
                metric_distance_computations += size;
            }

            // pre-fetch the information into cache... 

            // iterate through each neighbor
            for (size_t j = 1; j <= size; j++) {
                int candidate_id = *(data + j);

                // pre-fetch the information into cache...

                // check if the point has been visited already! (tabu search)
                if (!(visited_array[candidate_id] == visited_array_tag)) {
                    visited_array[candidate_id] = visited_array_tag;

                    // compute distance to the object
                    char *currObj1 = (getDataByInternalId(candidate_id));
                    dist_t dist = fstdistfunc_(query_data, currObj1, dist_func_param_);

                    if (top_candidates.size() < ef || lowerBound > dist) {
                        candidate_set.emplace(-dist, candidate_id);
                        top_candidates.emplace(dist, candidate_id);
                        if (top_candidates.size() > ef) top_candidates.pop();
                        if (!top_candidates.empty()) lowerBound = top_candidates.top().first;
                    }
                }
            }
        }

        // release the visisted list
        visited_list_pool_->releaseVisitedList(vl);

        // only keep the k closest points
        while (top_candidates.size() > k) {
            top_candidates.pop();
        }
        while (top_candidates.size() > 0) {
            std::pair<dist_t, tableint> rez = top_candidates.top();
            result.push(std::pair<dist_t, labeltype>(rez.first, getExternalLabel(rez.second)));
            top_candidates.pop();
        }
        return result;
    }


    /**
     * 
     * @brief The Beam Search Algo by Chavez et al.
     * 
     * 
     * @param query_data 
     * @param k 
     * @return std::priority_queue<std::pair<dist_t, labeltype>> 
     */
    std::priority_queue<std::pair<dist_t, labeltype>> search_beam(const void *query_data, size_t k) const {
        std::priority_queue<std::pair<dist_t, labeltype>> result;
        if (cur_element_count == 0) return result;
        bool collect_metrics = true;

        // - initialize visited list for tabu search
        VisitedList *vl = visited_list_pool_->getFreeVisitedList();
        vl_type *visited_array = vl->mass;
        vl_type visited_array_tag = vl->curV;

        //  - initialize the knn and beam objects
        std::priority_queue<std::pair<dist_t, tableint>, std::vector<std::pair<dist_t, tableint>>, CompareByFirst>
            top_candidates;     // containing the k closest points: max_queue- keep at k
        std::priority_queue<std::pair<dist_t, tableint>, std::vector<std::pair<dist_t, tableint>>, CompareByFirst>
            candidate_set;      // containing the beam: min_queue

        // - initialize the kNN with random points
        size_t ef = std::max(ef_, k);
        for (size_t i = 0; i < ef; i++) {
            tableint random_node = (tableint) (rand() % cur_element_count);
            dist_t d = fstdistfunc_(query_data, getDataByInternalId(random_node), dist_func_param_);
            top_candidates.emplace(d, random_node);    
            if (top_candidates.size() > k) top_candidates.pop(); // delete the largest element
        }

        //  - repeat while result set improves
        bool flag_changed = true;
        while (flag_changed) {
            dist_t prev_furthest = top_candidates.top().first; // to measure improvement

            // - get the closest point in the top_candidates, as the last element (smallest)
            std::priority_queue<std::pair<dist_t, tableint>, std::vector<std::pair<dist_t, tableint>>, CompareByFirst>
                top_candidates_copy = top_candidates;
            while (top_candidates_copy.size() > 1) {
                candidate_set.emplace(-top_candidates_copy.top().first, top_candidates_copy.top().second);
                top_candidates_copy.pop();
            }
            // !!!! What if already visited?? Add all to beam! Not sure which ones are new

            // - insert this closest point into the beam
            while (candidate_set.size() > 0) {
                std::pair<dist_t, tableint> current_node_pair = candidate_set.top();
                candidate_set.pop();
                tableint current_node_id = current_node_pair.second;

                //  - tabu search check
                if (!(visited_array[current_node_id] == visited_array_tag)) {
                    visited_array[current_node_id] = visited_array_tag;

                    //  - gather neighbors
                    int *data = (int *)get_linklist0(current_node_id);
                    size_t size = getListCount((linklistsizeint *)data);
                    if (collect_metrics) {
                        metric_hops++;
                        metric_distance_computations += size;
                    }

                    //  - iterate through nexighbors
                    for (size_t j = 1; j <= size; j++) {
                        int candidate_id = *(data + j);

                        // check if the point has been visited already! (tabu search)
                        if (!(visited_array[candidate_id] == visited_array_tag)) {
                            visited_array[candidate_id] = visited_array_tag;
                            dist_t dist = fstdistfunc_(query_data, getDataByInternalId(candidate_id), dist_func_param_);

                            // if added to top k, then let's explore:
                            if (dist < top_candidates.top().first || top_candidates.size() < k) {
                                candidate_set.emplace(-dist, candidate_id);         
                                top_candidates.emplace(dist, candidate_id);          // add to list
                                if (top_candidates.size() > k) top_candidates.pop(); // remove largest
                            }
                        }
                    }
                }
            }

            // check for improvement
            if (prev_furthest >= top_candidates.top().first) flag_changed = false;
        }

        // release the visisted list
        visited_list_pool_->releaseVisitedList(vl);
        
        // only keep the k closest points
        while (top_candidates.size() > k) {
            top_candidates.pop();
        }
        while (top_candidates.size() > 0) {
            std::pair<dist_t, tableint> rez = top_candidates.top();
            result.push(std::pair<dist_t, labeltype>(rez.first, getExternalLabel(rez.second)));
            top_candidates.pop();
        }
        return result;
    }

};
}  // namespace hnswlib