#include "../../../include/libCacheSim/cache.h"
#include "../../../include/libCacheSim/cacheObj.h"
#include "../../../include/libCacheSim/evictionAlgo.h"
#include "../../../dataStructure/hashtable/hashtable.h"
#include "../../../dataStructure/minimalIncrementCBF.h"

#include <vector>
#include <map>
#define MAXFREQ 7

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
        uint64_t filter_limit;
        uint64_t core_limit;
        uint64_t staging_limit;
        uint64_t ghost_limit;
        double filter_size_ratio;
        double staging_size_ratio;
        double ghost_size_ratio;
        struct minimalIncrementCBF *CBF;
        int32_t epoch_count;
        int32_t epoch_update;
        double sketch_scale;
        //
        int32_t guard_freq;
        int32_t track_freq;
        std::vector<int32_t> hotdistribution;
        int32_t evictobj_num;
        int32_t seq_miss;
        int32_t ghost_freq;
        int32_t hit_on_ghost;
        int32_t move_to_core;
        request_t *req_staging;
        //
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
    } merlin_params_t;
} // namespace eviction

extern uint64_t track_id;

#ifdef __cplusplus
extern "C"
{
#endif

    static const char *DEFAULT_CACHE_PARAMS =
        "filter-size-ratio=0.10,staging-size-ratio=0.05,ghost-size-ratio=1.00,epoch-update=32,sckech-scale=1.0";

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
        int cbf_size = MIN(ccache_params.cache_size * 4 * params->sketch_scale, 1<<30);
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
        if(params->guard_freq != params->track_freq || (cache->n_req%1000000)==0){
            params->track_freq = params->guard_freq;
            printf("%ld merlin guard_freq: %d epoch guess: %d ", cache->n_req, params->guard_freq, params->filter2staging);
            printf("ghost2core: %d ghost2staging: %d filter2staging: %d filter2core: %d filter2ghost: %d staging2core: %d evict_staging_ghost: %d\n",
               params->ghost2core, params->ghost2staging, params->filter2staging, params->filter2core, params->filter2ghost, params->staging2core, params->evict_staging_ghost);
        }
        #endif
        return cache_get_base(cache, req);
    }
    static void printstatus(cache_t *cache);
    static cache_obj_t *addtoghost(cache_t *cache, const request_t *req, int freq);

    static cache_obj_t *merlin_find(cache_t *cache, const request_t *req,
                                   const bool update_cache)
    {
        auto *params = reinterpret_cast<eviction::merlin_params_t *>(cache->eviction_params);
        cache_obj_t *obj = NULL;
        if (!update_cache)
        {
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
#ifdef TRACK_PARAMETERS
int find_track = (req->obj_id == track_id);
#endif
        params->timer += 1;

        obj = params->filter->find(params->filter, req, update_cache);
        if (obj != NULL)
        {
            #ifdef TRACK_PARAMETERS
                if (find_track) {
                    printf("req %d %d find in filter\n",cache->n_req, req->obj_id);
                    }
            #endif
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
            #ifdef TRACK_PARAMETERS
    if (find_track) {
        printf("req %d %d find in staging\n",cache->n_req, req->obj_id);
        }
#endif
            params->staging_hitnum++;
            obj->MERLIN.staginghit = 1;
            if (obj->MERLIN.freq < MAXFREQ)
            {
                obj->MERLIN.freq++;
                params->hotdistribution[obj->MERLIN.freq] += 1;
            }
            params->seq_miss = 0;
            return obj;
        }
        obj = params->core->find(params->core, req, update_cache);
        if (obj != NULL)
        {
            #ifdef TRACK_PARAMETERS
    if (find_track) {
        printf("req %d %d find in core\n",cache->n_req, req->obj_id);
        }
#endif
            params->core_hitnum++;
            if (obj->MERLIN.freq < MAXFREQ)
            {
                obj->MERLIN.freq++;
                params->hotdistribution[obj->MERLIN.freq] += 1;
            }
            params->seq_miss = 0;
            return obj;
        }
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
            #ifdef TRACK_PARAMETERS
    if (find_track) {
        printf("req %d %d find in ghost move to core %d\n",cache->n_req, req->obj_id, params->move_to_core);
        }
#endif
        }
        #ifdef TRACK_PARAMETERS
    if (find_track) {
        printf("req %d %d miss in cache\n",cache->n_req, req->obj_id);
        }
#endif
        params->seq_miss ++;
        return NULL;
    }
    
    static void decreasepop(std::vector<int32_t> &hotdistribution, int32_t ori_freq, int32_t dest_freq)
    {
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
            // warmup
            obj = params->core->insert(params->core, req);
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
                    params->ghost2core++;
                    params->move_to_core = 0;
                    obj = params->core->insert(params->core, req);
                    obj->MERLIN.freq = params->ghost_freq;
                    params->ghost->remove(params->ghost, obj->obj_id);
                }
                else
                {
                    params->ghost2staging++;
                    minimalIncrementCBF_add(params->CBF, (void *)&req->obj_id, sizeof(obj_id_t));
                    obj = params->staging->insert(params->staging, req);
                    obj->MERLIN.freq = 0;
                    //inghost stagingpect
                    obj->MERLIN.inghost = 1;
                    params->hotdistribution[0]++;
                }
            }
            else
            {
                // new obj
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
        while (params->core_limit < params->core->get_occupied_byte(params->core))
        {
            cache_obj_t *obj_to_evict = params->core->to_evict(params->core, NULL);
            int ori_freq = obj_to_evict->MERLIN.freq;
            assert(ori_freq >= 0 && ori_freq <= MAXFREQ);
            copy_cache_obj_to_request(params->req_local, obj_to_evict);
            params->core->remove(params->core, obj_to_evict->obj_id);
            cache_obj_t *new_obj = params->staging->insert(params->staging, params->req_local);
            new_obj->MERLIN.freq = ori_freq;
            #ifdef TRACK_PARAMETERS
            int find_track = (obj_to_evict->obj_id == track_id);
            #endif
            #ifdef TRACK_PARAMETERS
            if (find_track) {
                printf("req %d obj %d move to staging\n",cache->n_req, obj_to_evict->obj_id);
                }
        #endif
        }
        return;
    }

    static void evict_staging(cache_t *cache)
    {
        auto params = reinterpret_cast<eviction::merlin_params_t *>(cache->eviction_params);
        cache_obj_t *staging_to_evict = params->staging->to_evict(params->staging, NULL);
        int ori_freq = staging_to_evict->MERLIN.freq;
        decreasepop(params->hotdistribution, ori_freq, -1);
        params->staging->remove(params->staging, staging_to_evict->obj_id);
        #ifdef TRACK_PARAMETERS
            int find_track = (staging_to_evict->obj_id == track_id);
            #endif
            #ifdef TRACK_PARAMETERS
            if (find_track) {
                printf("req %d obj %d removed from staging\n",cache->n_req, staging_to_evict->obj_id);
                }
        #endif
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
        int filter_value = minimalIncrementCBF_estimate(params->CBF, (void *)&filter_to_evict->obj_id,
                                                          sizeof(filter_to_evict->obj_id));
        if(filter_value > params->epoch_update){//false positive
            return 0;
        }
        int staging_value = minimalIncrementCBF_estimate(params->CBF, (void *)&staging_to_evict->obj_id,
                                                          sizeof(staging_to_evict->obj_id));
        if ( filter_value > staging_value){
            //add filter to staging and ghost (?)
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
                return 1;
                break;
            }
            // move object from staging to core
            #ifdef TRACK_PARAMETERS
            int find_track = (obj_to_evict->obj_id == track_id);
            #endif
            #ifdef TRACK_PARAMETERS
            if (find_track) {
                printf("req %d obj %d move to core from staging\n",cache->n_req, obj_to_evict->obj_id);
                }
        #endif
            int ori_freq = obj_to_evict->MERLIN.freq;
            copy_cache_obj_to_request(params->req_local, obj_to_evict);
            if(obj_to_evict->MERLIN.inghost){
                params->evict_staging_ghost++;
                removefromghost(cache, params->req_local);
                obj_to_evict->MERLIN.inghost = 0;
            }
            params->staging2core ++;
            params->staging->remove(params->staging, obj_to_evict->obj_id);
            // decreasepop(params->hotdistribution, ori_freq, 0);
            minimalIncrementCBF_add(params->CBF, (void *)&params->req_local->obj_id, sizeof(obj_id_t));
            cache_obj_t *new_obj = params->core->insert(params->core, params->req_local);
            new_obj->MERLIN.freq = ori_freq-1;
            decreasepop(params->hotdistribution, ori_freq, ori_freq-1);
        }
        return 0;
    }

    static int adjust_filter(cache_t *cache)
    {
        auto params = reinterpret_cast<eviction::merlin_params_t *>(cache->eviction_params);
        int has_evicted = 0;

        while (!has_evicted && params->filter->get_occupied_byte(params->filter) > 0)
        {
            cache_obj_t *obj_to_evict = params->filter->to_evict(params->filter, NULL);
            #ifdef TRACK_PARAMETERS
            int find_track = (obj_to_evict->obj_id == track_id);
            #endif
            int ori_freq = obj_to_evict->MERLIN.freq;
            copy_cache_obj_to_request(params->req_local, obj_to_evict);
            // add to cbf
            if (ori_freq >= params->guard_freq)
            {
                // move to core
                // decreasepop(params->hotdistribution, ori_freq, 0);
                params->filter2core++;
                cache_obj_t *new_obj = params->core->insert(params->core, params->req_local);
                new_obj->MERLIN.freq = 0;
                decreasepop(params->hotdistribution, ori_freq, 0);
                #ifdef TRACK_PARAMETERS
                    if (find_track) {
                        printf("req %d obj %d move to core from filter\n",cache->n_req, obj_to_evict->obj_id);
                        }
                #endif
            }
            else
            {
                has_evicted = 1;
                int ret = compare(cache);
                if(ret == 1){
                    //filter wins compare, evict staging
                    //object movement done in compare
                    params->filter2staging++;
                    evict_staging(cache);
                    #ifdef TRACK_PARAMETERS
                    if (find_track) {
                        printf("req %d obj %d move to staging from filter\n",cache->n_req, obj_to_evict->obj_id);
                        }
                #endif
                    return has_evicted;
                }else{
                    //evict filter
                    params->filter2ghost++;
                    cache_obj_t *new_obj = addtoghost(cache, params->req_local, ori_freq);
                    #ifdef TRACK_PARAMETERS
                    if (find_track) {
                        printf("req %d obj %d remove from filter\n",cache->n_req,  obj_to_evict->obj_id);
                        }
                #endif
                    
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
        // todo adjust guard_freq
        int guradfreq = params->guard_freq;
        // int guardthreshold = params->core_limit;
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
            params->evictobj_num = 0;
            params->epoch_count += 1;
            if ((params->epoch_count >= params->epoch_update) == 0)
            {
                params->epoch_count = 0;
                minimalIncrementCBF_decay(params->CBF);
                //printstatus(cache);
            }
        }

        merlin_adjustguard(cache);

        if(params->filter->get_occupied_byte(params->filter) + req->obj_size > params->filter_limit){
            //evict filter
            adjust_filter(cache);
        }else{
            //evict staging
            int ret = 0;
            while(ret == 0){
                adjust_core(cache);
                ret = adjust_staging(cache);
            }
            // is compareing needed?
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
            /* different parameters are separated by comma,
             * key and value are separated by = */
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
            else if (strcasecmp(key, "sckech-scale") == 0)
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
