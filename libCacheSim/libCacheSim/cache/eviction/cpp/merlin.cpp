#include "../../../include/libCacheSim/cache.h"
#include "../../../include/libCacheSim/cacheObj.h"
#include "../../../include/libCacheSim/evictionAlgo.h"
#include "../../../dataStructure/hashtable/hashtable.h"
#include "../../../dataStructure/minimalIncrementCBF.h"

#include <vector>
#include <map>
#define MAXFREQ 7

/*
 * Merlin is implemented as a composite eviction policy built from four FIFO
 * queues:
 *
 *   filter  : first-touch admission area for newly seen objects.
 *   staging : probationary area for objects that look promising but are not
 *             hot enough to protect in the core cache yet.
 *   core    : protected area for objects whose observed popular and hotness passes the
 *             adaptive guard threshold.
 *   ghost   : metadata-only history of recently evicted candidates.
 *
 * A minimal-increment counting Bloom filter (CBF) provides a compact
 * epoch signal across epochs. The adaptive guard frequency is derived from
 * hotdistribution[] and controls whether an object should be promoted into the
 * protected core or stay in the probationary path.
 */
namespace eviction
{
    typedef struct
    {
        int64_t timer = 0;
        request_t *req_local;
        cache_t *filter;
        cache_t *core;
        cache_t *staging;
        cache_t *ghost;
        // Byte budgets for the four internal queues.
        uint64_t filter_limit;
        uint64_t core_limit;
        uint64_t staging_limit;
        uint64_t ghost_limit;
        double filter_size_ratio;
        double staging_size_ratio;
        double ghost_size_ratio;
        // Compact history used to compare candidates and age past evidence.
        struct minimalIncrementCBF *CBF;
        int32_t epoch_count;
        int32_t epoch_update;
        double sketch_scale;
        // Adaptive admission state.
        int32_t guard_freq;
        int32_t track_freq;
        // hotdistribution[f] stores the number of objects with frequency >= f.
        std::vector<int32_t> hotdistribution;
        int32_t evictobj_num;
        int32_t seq_miss;
        // Sticky state set by merlin_find() and consumed by merlin_insert().
        int32_t ghost_freq;
        int32_t hit_on_ghost;
        int32_t move_to_core;
        request_t *req_staging;
        // Counters used for debugging and evaluation.
        int32_t filter_hitnum;
        int32_t staging_hitnum;
        int32_t core_hitnum;
        int32_t ghost_hitnum;
        int32_t compareguard;

        int32_t ghost2core;
        int32_t ghost2staging;
        int32_t filter2staging;
        int32_t filter2core;
        int32_t filter2ghost;
        int32_t staging2core;
        int32_t evict_staging_ghost;

        int64_t n_byte_admit_to_core;
        int64_t n_byte_move_to_core;
        #ifdef TRACK_PARAMETERS
        //used for analysis
        uint64_t obj2core;
        uint64_t obj2staging;
        uint64_t hitcore;
        uint64_t hitstaging;
        uint64_t hitobjcore;
        uint64_t hitobjstaging;
        #endif
    } merlin_params_t;
} // namespace eviction

#ifdef TRACK_PARAMETERS
    #ifdef OUTPUT_GAP
        int outputgap = OUTPUT_GAP;
    #else
        int outputgap = 10000;
    #endif
#endif

#ifdef __cplusplus
extern "C"
{
#endif

    static const char *DEFAULT_CACHE_PARAMS =
        "filter-size-ratio=0.10,staging-size-ratio=0.05,ghost-size-ratio=1.00,epoch-update=32,sketch-scale=1.0";

    cache_t *merlin_init(const common_cache_params_t ccache_params,
                        const char *cache_specific_params);
    static void merlin_free(cache_t *cache);
    static bool merlin_get(cache_t *cache, const request_t *req);

    static cache_obj_t *merlin_find(cache_t *cache, const request_t *req,
                                   const bool update_cache);
    static cache_obj_t *merlin_insert(cache_t *cache, const request_t *req);
    static cache_obj_t *merlin_to_evict(cache_t *cache, const request_t *req);
    static void merlin_evict(cache_t *cache, const request_t *req);
    static bool merlin_remove(cache_t *cache, const obj_id_t obj_id);
    static inline int64_t merlin_get_occupied_byte(const cache_t *cache);
    static inline int64_t merlin_get_n_obj(const cache_t *cache);
    static inline bool merlin_can_insert(cache_t *cache, const request_t *req);
    static void merlin_update(cache_t *cache);
    static void merlin_print(cache_t *cache);
    static void merlin_aging(cache_t *cache);
    static void merlin_parse_params(cache_t *cache,
                                   const char *cache_specific_params);

    cache_t *merlin_init(const common_cache_params_t ccache_params,
                        const char *cache_specific_params)
    {
        cache_t *cache = cache_struct_init("merlin", ccache_params, cache_specific_params);
        cache->eviction_params = reinterpret_cast<void *>(new eviction::merlin_params_t);

        cache->cache_init = merlin_init;
        cache->cache_free = merlin_free;
        cache->get = merlin_get;
        cache->find = merlin_find;
        cache->insert = merlin_insert;
        cache->evict = merlin_evict;
        cache->to_evict = merlin_to_evict;
        cache->remove = merlin_remove;
        cache->get_n_obj = merlin_get_n_obj;
        cache->get_occupied_byte = merlin_get_occupied_byte;
        cache->can_insert = merlin_can_insert;

        auto *params = reinterpret_cast<eviction::merlin_params_t *>(cache->eviction_params);

        params->req_local = new_request();
        params->req_staging = new_request();

        merlin_parse_params(cache, DEFAULT_CACHE_PARAMS);
        if (cache_specific_params != NULL)
        {
            merlin_parse_params(cache, cache_specific_params);
        }

        // Split the user-visible cache budget among Merlin's internal queues.
        // The ghost queue stores history and can be sized independently.
        params->filter_limit = MAX(1, (uint64_t)(params->filter_size_ratio * ccache_params.cache_size));
        params->staging_limit = MAX(1, (uint64_t)(params->staging_size_ratio * ccache_params.cache_size));
        params->core_limit = ccache_params.cache_size - params->filter_limit - params->staging_limit;
        params->ghost_limit = ccache_params.cache_size * params->ghost_size_ratio;
        common_cache_params_t ccache_params_filter = ccache_params;
        ccache_params_filter.cache_size = params->filter_limit;
        common_cache_params_t ccache_params_staging = ccache_params;
        ccache_params_staging.cache_size = params->staging_limit;
        common_cache_params_t ccache_params_core = ccache_params;
        ccache_params_core.cache_size = params->core_limit;
        common_cache_params_t ccache_params_ghost = ccache_params;
        ccache_params_ghost.cache_size = params->ghost_limit;
        params->filter = FIFO_init(ccache_params_filter, cache_specific_params);
        params->staging = FIFO_init(ccache_params_staging, cache_specific_params);
        params->core = FIFO_init(ccache_params_core, cache_specific_params);
        params->ghost = FIFO_init(ccache_params_ghost, cache_specific_params);
        params->hotdistribution = std::vector<int32_t>(MAXFREQ + 1, 0);
        params->epoch_count = 0;
        params->evictobj_num = 0;
        params->timer = 0;
        params->guard_freq = 2;
        params->track_freq = 2;
        params->compareguard = 0;
        params->seq_miss = 0;
        params->CBF =
            (struct minimalIncrementCBF *)malloc(sizeof(struct minimalIncrementCBF));
        params->CBF->ready = 0;
        // The sketch is capped to avoid excessive memory use on very large
        // cache configurations.
        int cbf_size = MIN(ccache_params.cache_size * params->sketch_scale, 1<<30);
        int ret = minimalIncrementCBF_init(params->CBF, cbf_size, 0.001);
        if (ret != 0)
        {
            ERROR("CBF init failed\n");
        }
        params->hit_on_ghost = 0;
        params->move_to_core = 0;
        params->filter_hitnum = 0;
        params->staging_hitnum = 0;
        params->core_hitnum = 0;
        params->ghost_hitnum = 0;

        params->ghost2core = 0;
        params->ghost2staging = 0;
        params->filter2staging = 0;
        params->filter2core = 0;
        params->filter2ghost = 0;
        params->staging2core = 0;
        params->evict_staging_ghost = 0;

        params->n_byte_move_to_core = 0;
        params->n_byte_admit_to_core = 0;
        #ifdef TRACK_PARAMETERS
        params->obj2core = 0;
        params->obj2staging = 0;
        params->hitcore = 0;
        params->hitstaging = 0;
        params->hitobjcore = 0;
        params->hitobjstaging = 0;
        #endif

        snprintf(cache->cache_name, CACHE_NAME_ARRAY_LEN, "merlin-%.2lf-%.2lf-%.2lf-%d-%.2f",
                 params->filter_size_ratio, params->staging_size_ratio,params->ghost_size_ratio,params->epoch_update,params->sketch_scale);
        return cache;
    }

    static void merlin_free(cache_t *cache)
    {
        auto *params = reinterpret_cast<eviction::merlin_params_t *>(cache->eviction_params);
        params->filter->cache_free(params->filter);
        params->staging->cache_free(params->staging);
        params->core->cache_free(params->core);
        params->ghost->cache_free(params->ghost);
        minimalIncrementCBF_free(params->CBF);
        free(params->CBF);
        free_request(params->req_local);
        delete reinterpret_cast<eviction::merlin_params_t *>(cache->eviction_params);
        cache_struct_free(cache);
    }

    static bool merlin_get(cache_t *cache, const request_t *req)
    {
        #ifdef TRACK_PARAMETERS
        auto *params = reinterpret_cast<eviction::merlin_params_t *>(cache->eviction_params);
        if(params->guard_freq != params->track_freq || (cache->n_req%outputgap)==0){
            params->track_freq = params->guard_freq;
            printf("%ld merlin guard_freq: %d epoch guess: %d ", cache->n_req, params->guard_freq, params->filter2staging);
            printf("obj2core: %lu average hit in core: %lf precision %lf\n", params->obj2core, params->hitobjcore==0?0:(double)params->hitcore/params->hitobjcore, params->obj2core==0?0:(double)params->hitobjcore/params->obj2core);
        }
        #endif
        return cache_get_base(cache, req);
    }
    static void printstatus(cache_t *cache);
    static cache_obj_t *addtoghost(cache_t *cache, const request_t *req, int freq);

    static void checkhit(cache_t *cache, cache_obj_t * obj){
        #ifdef TRACK_PARAMETERS
        auto *params = reinterpret_cast<eviction::merlin_params_t *>(cache->eviction_params);
        if(obj->MERLIN.tocore){
            params->hitcore++;
            if(obj->MERLIN.accessed==0){
                params->hitobjcore++;
                obj->MERLIN.accessed=1;
            }
        }
        if(obj->MERLIN.tostaging){
            params->hitstaging++;
            if(obj->MERLIN.accessed==0){
                params->hitobjstaging++;
                obj->MERLIN.accessed=1;
            }
        }
        #endif
        return;
    }

    static cache_obj_t *merlin_find(cache_t *cache, const request_t *req,
                                   const bool update_cache)
    {
        auto *params = reinterpret_cast<eviction::merlin_params_t *>(cache->eviction_params);
        cache_obj_t *obj = NULL;
        if (!update_cache)
        {
            // Lookup-only path: do not update frequencies, hit counters, or
            // ghost-hit state.
            obj = params->filter->find(params->filter, req, update_cache);
            if (obj != NULL)
            {
                return obj;
            }
            obj = params->staging->find(params->staging, req, update_cache);
            if (obj != NULL)
            {
                return obj;
            }
            obj = params->core->find(params->core, req, update_cache);
            if (obj != NULL)
            {
                return obj;
            }
            return NULL;
        }

        params->timer += 1;

        // Search real cache partitions first. A hit increases the object's
        // bounded frequency and updates the cumulative frequency histogram.
        obj = params->filter->find(params->filter, req, update_cache);
        if (obj != NULL)
        {
            params->filter_hitnum++;
            if (obj->MERLIN.freq < MAXFREQ)
            {
                obj->MERLIN.freq++;
                params->hotdistribution[obj->MERLIN.freq] += 1;
            }
            params->seq_miss = 0;
            return obj;
        }
        obj = params->staging->find(params->staging, req, update_cache);
        if (obj != NULL)
        {
            params->staging_hitnum++;
            obj->MERLIN.staginghit = 1;
            if (obj->MERLIN.freq < MAXFREQ)
            {
                obj->MERLIN.freq++;
                params->hotdistribution[obj->MERLIN.freq] += 1;
            }
            params->seq_miss = 0;
            checkhit(cache, obj);
            return obj;
        }
        obj = params->core->find(params->core, req, update_cache);
        if (obj != NULL)
        {
            params->core_hitnum++;
            if (obj->MERLIN.freq < MAXFREQ)
            {
                obj->MERLIN.freq++;
                params->hotdistribution[obj->MERLIN.freq] += 1;
            }
            params->seq_miss = 0;
            checkhit(cache, obj);
            return obj;
        }
        // Ghost hits are misses from the user's point of view, but they carry
        // useful history. merlin_insert() consumes this state to decide whether
        // the returning object enters staging or core.
        obj = params->ghost->find(params->ghost, req, false);
        if (obj != NULL)
        {
            params->ghost_hitnum++;
            if (obj->MERLIN.freq < MAXFREQ)
            {
                obj->MERLIN.freq++;
                params->hotdistribution[obj->MERLIN.freq] += 1;
            }
            params->hit_on_ghost = true;
            params->ghost_freq = obj->MERLIN.freq;
            if (obj->MERLIN.freq >= params->guard_freq)
            {
                params->move_to_core = true;
            }
        }
        params->seq_miss ++;
        return NULL;
    }
    
    static void decreasepop(std::vector<int32_t> &hotdistribution, int32_t ori_freq, int32_t dest_freq)
    {
        // hotdistribution is cumulative: moving an object from ori_freq to
        // dest_freq removes it from every threshold it no longer satisfies.
        for (int i = ori_freq; i > dest_freq; i--)
        {
            hotdistribution[i]--;
        }
    }

    static cache_obj_t *merlin_insert(cache_t *cache, const request_t *req)
    {
        auto *params = reinterpret_cast<eviction::merlin_params_t *>(cache->eviction_params);
        cache_obj_t *obj = NULL;
        if (params->staging->get_occupied_byte(params->staging) == 0 && params->filter->get_occupied_byte(params->filter) == 0)
        {
            // Warmup: before the admission queues contain enough signal, admit
            // directly to core so the cache can start serving hits immediately.
            obj = params->core->insert(params->core, req);
            params->n_byte_admit_to_core += obj -> obj_size;
            obj->MERLIN.freq = 0;
            params->hotdistribution[0]++;
        }
        else
        {
            if (params->hit_on_ghost)
            {
                params->hit_on_ghost = 0;
                if (params->move_to_core)
                {
                    // A returning ghost object whose historical frequency has
                    // crossed guard_freq is treated as hot and protected.
                    params->ghost2core++;
                    params->move_to_core = 0;
                    obj = params->core->insert(params->core, req);
                    obj->MERLIN.freq = params->ghost_freq;
                    params->ghost->remove(params->ghost, obj->obj_id);
                    #ifdef TRACK_PARAMETERS
                    params->obj2core++;
                    obj->MERLIN.tocore = 1;
                    #endif
                }
                else
                {
                    // A ghost hit below the guard threshold is promising but
                    // still probationary, so it enters staging and is tracked
                    // in the sketch.
                    params->ghost2staging++;
                    minimalIncrementCBF_add(params->CBF, (void *)&req->obj_id, sizeof(obj_id_t));
                    obj = params->staging->insert(params->staging, req);
                    params->n_byte_admit_to_core += obj -> obj_size; 
                    obj->MERLIN.freq = 0;
                    //inghost stagingpect
                    obj->MERLIN.inghost = 1;
                    params->hotdistribution[0]++;
                    #ifdef TRACK_PARAMETERS
                    params->obj2staging++;
                    obj->MERLIN.tostaging = 1;
                    #endif
                }
            }
            else
            {
                // First-seen objects enter the filter. They must earn further
                // protection through hits or sketch evidence.
                obj = params->filter->insert(params->filter, req);
                obj->MERLIN.freq = 0;
                params->hotdistribution[0]++;
            }
        }
        return obj;
    }

    static cache_obj_t *merlin_to_evict(cache_t *cache, const request_t *req)
    {
        auto params = reinterpret_cast<eviction::merlin_params_t *>(cache->eviction_params);
        assert(0);
        return NULL;
    }

    static cache_obj_t *addtoghost(cache_t *cache, const request_t *req, int freq)
    {
        auto params = reinterpret_cast<eviction::merlin_params_t *>(cache->eviction_params);
        cache_obj_t *object = params->ghost->insert(params->ghost, req);
        object->MERLIN.freq = freq;
        return object;
    }

    static int removefromghost(cache_t *cache, const request_t *req)
    {
        auto params = reinterpret_cast<eviction::merlin_params_t *>(cache->eviction_params);
        cache_obj_t *obj = params->ghost->find(params->ghost, req, false);
        if (obj != NULL)
        {
            int ori_freq = obj->MERLIN.freq;
            decreasepop(params->hotdistribution, ori_freq, -1);
            params->ghost->remove(params->ghost, obj->obj_id);
            return ori_freq;
        }
        return 0;
    }

    static void adjust_core(cache_t *cache)
    {
        auto params = reinterpret_cast<eviction::merlin_params_t *>(cache->eviction_params);
        // If core exceeds its budget, demote its FIFO victims to staging. This
        // preserves their metadata while making room for newly promoted items.
        while (params->core_limit < params->core->get_occupied_byte(params->core))
        {
            cache_obj_t *obj_to_evict = params->core->to_evict(params->core, NULL);
            int ori_freq = obj_to_evict->MERLIN.freq;
            assert(ori_freq >= 0 && ori_freq <= MAXFREQ);
            copy_cache_obj_to_request(params->req_local, obj_to_evict);
            cache_obj_t *new_obj = params->staging->insert(params->staging, params->req_local);
            new_obj->MERLIN.freq = ori_freq;
            #ifdef TRACK_PARAMETERS
            //inherit state
            new_obj->MERLIN.tocore = obj_to_evict->MERLIN.tocore;
            new_obj->MERLIN.tostaging = obj_to_evict->MERLIN.tostaging;
            new_obj->MERLIN.accessed = obj_to_evict->MERLIN.accessed;
            #endif
            params->core->remove(params->core, obj_to_evict->obj_id);
        }
        return;
    }

    static void evict_staging(cache_t *cache)
    {
        auto params = reinterpret_cast<eviction::merlin_params_t *>(cache->eviction_params);
        // Staging eviction drops probationary objects and removes their
        // contribution from the cumulative hotness distribution.
        cache_obj_t *staging_to_evict = params->staging->to_evict(params->staging, NULL);
        int ori_freq = staging_to_evict->MERLIN.freq;
        decreasepop(params->hotdistribution, ori_freq, -1);
        params->staging->remove(params->staging, staging_to_evict->obj_id);
    }

    static int compare(cache_t *cache){
        auto params = reinterpret_cast<eviction::merlin_params_t *>(cache->eviction_params);
        cache_obj_t *filter_to_evict = params->filter->to_evict(params->filter, NULL);
        cache_obj_t *staging_to_evict = params->staging->to_evict(params->staging, NULL);
        if(filter_to_evict==NULL||staging_to_evict==NULL){
            return 0;
        }
        if(staging_to_evict->MERLIN.freq > 0){
            return 0;
        }
        // Compare the oldest filter candidate with the oldest cold staging
        // candidate using sketch estimates. A higher sketch value means the
        // filter object has stronger recent history and can replace staging.
        int filter_value = minimalIncrementCBF_estimate(params->CBF, (void *)&filter_to_evict->obj_id,
                                                          sizeof(filter_to_evict->obj_id));
        if(filter_value > params->epoch_update){//false positive
            return 0;
        }
        int staging_value = minimalIncrementCBF_estimate(params->CBF, (void *)&staging_to_evict->obj_id,
                                                          sizeof(staging_to_evict->obj_id));
        if ( filter_value > staging_value){
            // The filter candidate wins: move it into staging and remember it
            // in ghost so a future return can reuse this frequency state.
            params->compareguard++;
            minimalIncrementCBF_add(params->CBF, (void *)&filter_to_evict->obj_id, sizeof(obj_id_t));
            copy_cache_obj_to_request(params->req_staging, filter_to_evict);
            cache_obj_t *new_obj = params->staging->insert(params->staging, params->req_staging);
            new_obj->MERLIN.freq = 0;
            new_obj->MERLIN.inghost = 1;
            params->hotdistribution[new_obj->MERLIN.freq]++;
            cache_obj_t *new_obj_ghost = addtoghost(cache, params->req_staging, filter_to_evict->MERLIN.freq);
            new_obj_ghost->MERLIN.freq = filter_to_evict->MERLIN.freq;
            params->filter->remove(params->filter, filter_to_evict->obj_id);
            params->n_byte_move_to_core += filter_to_evict->obj_size;
            #ifdef TRACK_PARAMETERS
            params->obj2staging++;
            new_obj->MERLIN.tostaging = 1;
            #endif
            return 1;
        }
        return 2;
    }

    static int adjust_staging(cache_t *cache){
        auto params = reinterpret_cast<eviction::merlin_params_t *>(cache->eviction_params);
        while (params->staging->get_occupied_byte(params->staging) > 0)
        {
            cache_obj_t *obj_to_evict = params->staging->to_evict(params->staging, NULL);
            if(obj_to_evict->MERLIN.freq==0){
                // Stop at the first cold staging object. The caller can evict
                // it or compare it against a filter candidate.
                return 1;
                break;
            }
            // Hot staging objects graduate to core. Frequency is decremented by
            // one to charge a promotion cost and avoid over-protecting bursts.
            int ori_freq = obj_to_evict->MERLIN.freq;
            copy_cache_obj_to_request(params->req_local, obj_to_evict);
            if(obj_to_evict->MERLIN.inghost){
                params->evict_staging_ghost++;
                removefromghost(cache, params->req_local);
                obj_to_evict->MERLIN.inghost = 0;
            }
            // decreasepop(params->hotdistribution, ori_freq, 0);
            minimalIncrementCBF_add(params->CBF, (void *)&params->req_local->obj_id, sizeof(obj_id_t));
            cache_obj_t *new_obj = params->core->insert(params->core, params->req_local);
            new_obj->MERLIN.freq = ori_freq-1;
            decreasepop(params->hotdistribution, ori_freq, ori_freq-1);
            #ifdef TRACK_PARAMETERS
            //inherit state
            new_obj->MERLIN.tocore = obj_to_evict->MERLIN.tocore;
            new_obj->MERLIN.tostaging = obj_to_evict->MERLIN.tostaging;
            new_obj->MERLIN.accessed = obj_to_evict->MERLIN.accessed;
            #endif
            params->staging2core ++;
            params->staging->remove(params->staging, obj_to_evict->obj_id);
        }
        return 0;
    }

    static int adjust_filter(cache_t *cache)
    {
        auto params = reinterpret_cast<eviction::merlin_params_t *>(cache->eviction_params);
        int has_evicted = 0;

        // Make room in filter. Hot filter victims bypass staging and enter
        // core; cold victims are either compared against staging or recorded in
        // ghost before being removed.
        while (!has_evicted && params->filter->get_occupied_byte(params->filter) > 0)
        {
            cache_obj_t *obj_to_evict = params->filter->to_evict(params->filter, NULL);
            int ori_freq = obj_to_evict->MERLIN.freq;
            copy_cache_obj_to_request(params->req_local, obj_to_evict);
            if (ori_freq >= params->guard_freq)
            {
                params->filter2core++;
                cache_obj_t *new_obj = params->core->insert(params->core, params->req_local);
                new_obj->MERLIN.freq = 0;
                decreasepop(params->hotdistribution, ori_freq, 0);
            }
            else
            {
                has_evicted = 1;
                int ret = compare(cache);
                if(ret == 1){
                    // compare() has already moved the filter candidate; evict
                    // the losing staging candidate to complete the swap.
                    params->filter2staging++;
                    evict_staging(cache);
                    return has_evicted;
                }else{
                    // The filter candidate loses or cannot be compared; keep
                    // only its metadata in ghost.
                    params->filter2ghost++;
                    cache_obj_t *new_obj = addtoghost(cache, params->req_local, ori_freq);                    
                }
            }
            minimalIncrementCBF_add(params->CBF, (void *)&params->req_local->obj_id, sizeof(obj_id_t));
            params->filter->remove(params->filter, obj_to_evict->obj_id);
        }
        return has_evicted;
    }

    static void adjust_ghost(cache_t *cache)
    {
        auto params = reinterpret_cast<eviction::merlin_params_t *>(cache->eviction_params);
        // Bound ghost memory and remove stale metadata from hotdistribution.
        while (params->ghost->get_occupied_byte(params->ghost) > params->ghost_limit)
        {
            cache_obj_t *obj_to_evict = params->ghost->to_evict(params->ghost, NULL);
            int ori_freq = obj_to_evict->MERLIN.freq;
            params->ghost->remove(params->ghost, obj_to_evict->obj_id);
            decreasepop(params->hotdistribution, ori_freq, -1);
        }
        return;
    }

    static void merlin_adjustguard(cache_t *cache)
    {
        auto params = reinterpret_cast<eviction::merlin_params_t *>(cache->eviction_params);
        // Choose the smallest frequency threshold whose population roughly
        // matches the protected core population. This adapts the promotion
        // boundary as the workload gets hotter or colder.
        int guradfreq = params->guard_freq;
        int guardthreshold = params->core->get_n_obj(params->core);
        if (params->hotdistribution[guradfreq] > guardthreshold)
        {
            while (params->hotdistribution[guradfreq] > guardthreshold)
            {
                guradfreq++;
            }
        }
        else
        {
            while (guradfreq > 1 && params->hotdistribution[guradfreq - 1] < guardthreshold)
            {
                guradfreq--;
            }
        }
        params->guard_freq = guradfreq;
        return;
    }

    static void printstatus(cache_t *cache)
    {
        auto params = reinterpret_cast<eviction::merlin_params_t *>(cache->eviction_params);
        printf("filter: %ld staging: %ld core: %ld ghost: %ld\n",
               params->filter->get_occupied_byte(params->filter),
               params->staging->get_occupied_byte(params->staging),
               params->core->get_occupied_byte(params->core),
               params->ghost->get_occupied_byte(params->ghost));
        printf("epoch_count %d gurad: %d ", params->epoch_count, params->guard_freq);
        printf("hotdistribution: %d\n", params->guard_freq);
        for (auto &pop : params->hotdistribution)
        {
            printf("%d ", pop);
        }
        printf("\n");
        printf("filter_hitnum: %d staging_hitnum: %d core_hitnum: %d ghost_hitnum: %d\n",
               params->filter_hitnum, params->staging_hitnum, params->core_hitnum, params->ghost_hitnum);
        printf("ghost2core: %d ghost2staging: %d filter2staging: %d filter2core: %d filter2ghost: %d staging2core: %d evict_staging_ghost: %d\n",
               params->ghost2core, params->ghost2staging, params->filter2staging, params->filter2core, params->filter2ghost, params->staging2core, params->evict_staging_ghost);
        printf("compareguard: %d\n\n", params->compareguard);
        return;
    }

    static void merlin_evict(cache_t *cache, const request_t *req)
    {
        if (cache == nullptr || cache->eviction_params == nullptr)
        {
            fprintf(stderr, "Error: cache or cache->eviction_params is null\n");
            return;
        }

        auto params = reinterpret_cast<eviction::merlin_params_t *>(cache->eviction_params);
        params->evictobj_num += 1;
        if (params->evictobj_num == cache->get_n_obj(cache))
        {
            // Treat one full-cache worth of evictions as an epoch. Periodic
            // sketch decay prevents old popularity from dominating forever.
            params->evictobj_num = 0;
            params->epoch_count += 1;
            if ((params->epoch_count >= params->epoch_update))
            {
                params->epoch_count = 0;
                minimalIncrementCBF_decay(params->CBF);
            }
        }

        merlin_adjustguard(cache);

        if(params->filter->get_occupied_byte(params->filter) + req->obj_size > params->filter_limit){
            // New insert would overflow filter, so free space from the
            // first-touch admission path.
            adjust_filter(cache);
        }else{
            // Otherwise free space from staging. Hot staging objects may be
            // promoted first, which can in turn require core demotion.
            int ret = 0;
            while(ret == 0){
                adjust_core(cache);
                ret = adjust_staging(cache);
            }
            // Give an old filter candidate one chance to replace the cold
            // staging victim before evicting staging.
            compare(cache);
            evict_staging(cache);
        }
        adjust_ghost(cache);
        return;
    }

    static bool merlin_remove(cache_t *cache, const obj_id_t obj_id)
    {
        auto params = reinterpret_cast<eviction::merlin_params_t *>(cache->eviction_params);
        bool removed = false;
        removed = removed || params->filter->remove(params->filter, obj_id);
        removed = removed || params->staging->remove(params->staging, obj_id);
        removed = removed || params->core->remove(params->core, obj_id);
        removed = removed || params->ghost->remove(params->ghost, obj_id);
        return removed;
        return true;
    }

    static inline int64_t merlin_get_occupied_byte(const cache_t *cache)
    {
        auto params = reinterpret_cast<eviction::merlin_params_t *>(cache->eviction_params);
        return params->filter->get_occupied_byte(params->filter) +
               params->staging->get_occupied_byte(params->staging) +
               params->core->get_occupied_byte(params->core);
        return cache_get_occupied_byte_default(cache);
    }

    static inline int64_t merlin_get_n_obj(const cache_t *cache)
    {
        auto params = reinterpret_cast<eviction::merlin_params_t *>(cache->eviction_params);
        return params->filter->get_n_obj(params->filter) +
               params->staging->get_n_obj(params->staging) +
               params->core->get_n_obj(params->core);
        return cache_get_n_obj_default(cache);
    }

    static inline bool merlin_can_insert(cache_t *cache, const request_t *req)
    {
        auto params = reinterpret_cast<eviction::merlin_params_t *>(cache->eviction_params);
        return req->obj_size <= params->filter->cache_size;
        return cache_can_insert_default(cache, req);
    }

    static void merlin_parse_params(cache_t *cache,
                                   const char *cache_specific_params)
    {
        auto *params = reinterpret_cast<eviction::merlin_params_t *>(cache->eviction_params);

        char *params_str = strdup(cache_specific_params);
        char *old_params_str = params_str;
        char *end;

        while (params_str != NULL && params_str[0] != '\0')
        {
            /* Parameters are encoded as comma-separated key=value pairs. */
            char *key = strsep((char **)&params_str, "=");
            char *value = strsep((char **)&params_str, ",");

            // skip the white space
            while (params_str != NULL && *params_str == ' ')
            {
                params_str++;
            }

            if (strcasecmp(key, "filter-size-ratio") == 0)
            {
                params->filter_size_ratio = strtod(value, NULL);
            }
            else if (strcasecmp(key, "staging-size-ratio") == 0)
            {
                params->staging_size_ratio = strtod(value, NULL);
            }
            else if (strcasecmp(key, "ghost-size-ratio") == 0)
            {
                params->ghost_size_ratio = strtod(value, NULL);
            }
            else if (strcasecmp(key, "epoch-update") == 0)
            {
                params->epoch_update = strtol(value, NULL, 10);
            }
            else if (strcasecmp(key, "sketch-scale") == 0)
            {
                params->sketch_scale = strtod(value, NULL);
            }
            else
            {
                ERROR("%s does not have parameter %s\n", cache->cache_name, key);
                exit(1);
            }
        }

        free(old_params_str);
    }

#ifdef __cplusplus
}
#endif
