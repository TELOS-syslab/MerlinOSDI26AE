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
        cache_t *main;
        cache_t *sus;
        cache_t *ghost;
        uint64_t filter_limit;
        uint64_t main_limit;
        uint64_t sus_limit;
        uint64_t ghost_limit;
        double filter_size_ratio;
        double suspected_size_ratio;
        double ghost_size_ratio;
        struct minimalIncrementCBF *CBF;
        //
        int32_t guard_freq;
        std::vector<int32_t> hotdistribution;
        int32_t phase;
        int32_t evictobj_num;
        int32_t seq_miss;
        int32_t ghost_freq;
        int32_t hit_on_ghost;
        int32_t move_to_main;
        int32_t prefetch;
        int32_t prefetch_freq;
        request_t *req_prefetch;
        request_t *req_sus;
        //
        int32_t filter_hitnum;
        int32_t sus_hitnum;
        int32_t main_hitnum;
        int32_t ghost_hitnum;
        int32_t compareguard;

        int32_t ghost2main;
        int32_t ghost2sus;
        int32_t filter2sus;
        int32_t filter2main;
        int32_t filter2ghost;
        int32_t sus2main;
        int32_t evict_sus_ghost;
    } flexs_params_t;
} // namespace eviction

#ifdef __cplusplus
extern "C"
{
#endif

    static const char *DEFAULT_CACHE_PARAMS =
        "filter-size-ratio=0.10,suspected-size-ratio=0.05,ghost-size-ratio=1.00";

    cache_t *flexs_init(const common_cache_params_t ccache_params,
                        const char *cache_specific_params);
    static void flexs_free(cache_t *cache);
    static bool flexs_get(cache_t *cache, const request_t *req);

    static cache_obj_t *flexs_find(cache_t *cache, const request_t *req,
                                   const bool update_cache);
    static cache_obj_t *flexs_insert(cache_t *cache, const request_t *req);
    static cache_obj_t *flexs_to_evict(cache_t *cache, const request_t *req);
    static void flexs_evict(cache_t *cache, const request_t *req);
    static bool flexs_remove(cache_t *cache, const obj_id_t obj_id);
    static inline int64_t flexs_get_occupied_byte(const cache_t *cache);
    static inline int64_t flexs_get_n_obj(const cache_t *cache);
    static inline bool flexs_can_insert(cache_t *cache, const request_t *req);
    static void flexs_update(cache_t *cache);
    static void flexs_print(cache_t *cache);
    static void flexs_aging(cache_t *cache);
    static void flexs_parse_params(cache_t *cache,
                                   const char *cache_specific_params);

    cache_t *flexs_init(const common_cache_params_t ccache_params,
                        const char *cache_specific_params)
    {
        cache_t *cache = cache_struct_init("flexs", ccache_params, cache_specific_params);
        cache->eviction_params = reinterpret_cast<void *>(new eviction::flexs_params_t);

        cache->cache_init = flexs_init;
        cache->cache_free = flexs_free;
        cache->get = flexs_get;
        cache->find = flexs_find;
        cache->insert = flexs_insert;
        cache->evict = flexs_evict;
        cache->to_evict = flexs_to_evict;
        cache->remove = flexs_remove;
        cache->get_n_obj = flexs_get_n_obj;
        cache->get_occupied_byte = flexs_get_occupied_byte;
        cache->can_insert = flexs_can_insert;

        auto *params = reinterpret_cast<eviction::flexs_params_t *>(cache->eviction_params);

        params->req_local = new_request();
        params->req_prefetch = new_request();
        params->req_sus = new_request();

        flexs_parse_params(cache, DEFAULT_CACHE_PARAMS);
        if (cache_specific_params != NULL)
        {
            flexs_parse_params(cache, cache_specific_params);
        }

        params->filter_limit = MAX(1, (uint64_t)(params->filter_size_ratio * ccache_params.cache_size));
        params->sus_limit = MAX(1, (uint64_t)(params->suspected_size_ratio * ccache_params.cache_size));
        params->main_limit = ccache_params.cache_size - params->filter_limit - params->sus_limit;
        params->ghost_limit = ccache_params.cache_size * params->ghost_size_ratio;
        common_cache_params_t ccache_params_filter = ccache_params;
        ccache_params_filter.cache_size = params->filter_limit;
        common_cache_params_t ccache_params_sus = ccache_params;
        ccache_params_sus.cache_size = params->sus_limit;
        common_cache_params_t ccache_params_main = ccache_params;
        ccache_params_main.cache_size = params->main_limit;
        common_cache_params_t ccache_params_ghost = ccache_params;
        ccache_params_ghost.cache_size = params->ghost_limit;
        params->filter = FIFO_init(ccache_params_filter, cache_specific_params);
        params->sus = FIFO_init(ccache_params_sus, cache_specific_params);
        params->main = FIFO_init(ccache_params_main, cache_specific_params);
        params->ghost = FIFO_init(ccache_params_ghost, cache_specific_params);
        params->hotdistribution = std::vector<int32_t>(MAXFREQ + 1, 0);
        params->phase = 0;
        params->evictobj_num = 0;
        params->timer = 0;
        params->guard_freq = 2;
        params->compareguard = 0;
        params->seq_miss = 0;
        params->CBF =
            (struct minimalIncrementCBF *)malloc(sizeof(struct minimalIncrementCBF));
        params->CBF->ready = 0;
        int cbf_size = MIN(ccache_params.cache_size * 4, 1<<30);
        int ret = minimalIncrementCBF_init(params->CBF, cbf_size, 0.001);
        if (ret != 0)
        {
            ERROR("CBF init failed\n");
        }
        params->hit_on_ghost = 0;
        params->move_to_main = 0;
        params->filter_hitnum = 0;
        params->sus_hitnum = 0;
        params->main_hitnum = 0;
        params->ghost_hitnum = 0;

        params->ghost2main = 0;
        params->ghost2sus = 0;
        params->filter2sus = 0;
        params->filter2main = 0;
        params->filter2ghost = 0;
        params->sus2main = 0;
        params->evict_sus_ghost = 0;
        params->prefetch = 0;

        snprintf(cache->cache_name, CACHE_NAME_ARRAY_LEN, "flexs-%.2lf-%.2lf-%.2lf",
                 params->filter_size_ratio, params->suspected_size_ratio,params->ghost_size_ratio);
        return cache;
    }

    static void flexs_free(cache_t *cache)
    {
        auto *params = reinterpret_cast<eviction::flexs_params_t *>(cache->eviction_params);
        params->filter->cache_free(params->filter);
        params->sus->cache_free(params->sus);
        params->main->cache_free(params->main);
        params->ghost->cache_free(params->ghost);
        minimalIncrementCBF_free(params->CBF);
        free(params->CBF);
        free_request(params->req_local);
        free_request(params->req_prefetch);
        delete reinterpret_cast<eviction::flexs_params_t *>(cache->eviction_params);
        cache_struct_free(cache);
    }

    static bool flexs_get(cache_t *cache, const request_t *req)
    {
        return cache_get_base(cache, req);
    }
    static void printstatus(cache_t *cache);
    static cache_obj_t *addtoghost(cache_t *cache, const request_t *req, int freq);

    static cache_obj_t *flexs_find(cache_t *cache, const request_t *req,
                                   const bool update_cache)
    {
        auto *params = reinterpret_cast<eviction::flexs_params_t *>(cache->eviction_params);
        cache_obj_t *obj = NULL;
        if (!update_cache)
        {
            obj = params->filter->find(params->filter, req, update_cache);
            if (obj != NULL)
            {
                return obj;
            }
            obj = params->sus->find(params->sus, req, update_cache);
            if (obj != NULL)
            {
                return obj;
            }
            obj = params->main->find(params->main, req, update_cache);
            if (obj != NULL)
            {
                return obj;
            }
            return NULL;
        }
        params->timer += 1;

        obj = params->filter->find(params->filter, req, update_cache);
        if (obj != NULL)
        {
            params->filter_hitnum++;
            if (obj->FLEX.freq < MAXFREQ)
            {
                obj->FLEX.freq++;
                params->hotdistribution[obj->FLEX.freq] += 1;
            }
            params->seq_miss = 0;
            return obj;
        }
        obj = params->sus->find(params->sus, req, update_cache);
        if (obj != NULL)
        {
            params->sus_hitnum++;
            obj->FLEX.sushit = 1;
            if (obj->FLEX.freq < MAXFREQ)
            {
                obj->FLEX.freq++;
                params->hotdistribution[obj->FLEX.freq] += 1;
            }
            params->seq_miss = 0;
            return obj;
        }
        obj = params->main->find(params->main, req, update_cache);
        if (obj != NULL)
        {
            params->main_hitnum++;
            if (obj->FLEX.freq < MAXFREQ)
            {
                obj->FLEX.freq++;
                params->hotdistribution[obj->FLEX.freq] += 1;
            }
            params->seq_miss = 0;
            return obj;
        }
        obj = params->ghost->find(params->ghost, req, false);
        if (obj != NULL)
        {
            params->ghost_hitnum++;
            if (obj->FLEX.freq < MAXFREQ)
            {
                obj->FLEX.freq++;
                params->hotdistribution[obj->FLEX.freq] += 1;
            }
            params->hit_on_ghost = true;
            params->ghost_freq = obj->FLEX.freq;
            if (obj->FLEX.freq >= params->guard_freq)
            {
                params->move_to_main = true;
            }
        }
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

    static cache_obj_t *flexs_insert(cache_t *cache, const request_t *req)
    {
        auto *params = reinterpret_cast<eviction::flexs_params_t *>(cache->eviction_params);
        cache_obj_t *obj = NULL;
        if (params->sus->get_occupied_byte(params->sus) == 0 && params->filter->get_occupied_byte(params->filter) == 0)
        {
            // warmup
            obj = params->main->insert(params->main, req);
            obj->FLEX.freq = 0;
            params->hotdistribution[0]++;
        }
        else
        {
            if (params->hit_on_ghost)
            {
                params->hit_on_ghost = 0;
                if (params->move_to_main)
                {
                    params->ghost2main++;
                    params->move_to_main = 0;
                    obj = params->main->insert(params->main, req);
                    obj->FLEX.freq = params->ghost_freq;
                    params->ghost->remove(params->ghost, obj->obj_id);
                }
                else
                {
                    params->ghost2sus++;
                    minimalIncrementCBF_add(params->CBF, (void *)&req->obj_id, sizeof(obj_id_t));
                    obj = params->sus->insert(params->sus, req);
                    obj->FLEX.freq = 0;
                    //inghost suspect
                    obj->FLEX.inghost = 1;
                    params->hotdistribution[0]++;
                }
            }
            else
            {
                // new obj
                obj = params->filter->insert(params->filter, req);
                obj->FLEX.freq = 0;
                params->hotdistribution[0]++;
            }
        }
        return obj;
    }

    static cache_obj_t *flexs_to_evict(cache_t *cache, const request_t *req)
    {
        auto params = reinterpret_cast<eviction::flexs_params_t *>(cache->eviction_params);
        assert(0);
        return NULL;
    }

    static cache_obj_t *addtoghost(cache_t *cache, const request_t *req, int freq)
    {
        auto params = reinterpret_cast<eviction::flexs_params_t *>(cache->eviction_params);
        cache_obj_t *object = params->ghost->insert(params->ghost, req);
        object->FLEX.freq = freq;
        return object;
    }

    static int removefromghost(cache_t *cache, const request_t *req)
    {
        auto params = reinterpret_cast<eviction::flexs_params_t *>(cache->eviction_params);
        cache_obj_t *obj = params->ghost->find(params->ghost, req, false);
        if (obj != NULL)
        {
            int ori_freq = obj->FLEX.freq;
            decreasepop(params->hotdistribution, ori_freq, -1);
            params->ghost->remove(params->ghost, obj->obj_id);
            return ori_freq;
        }
        return 0;
    }

    static void adjust_main(cache_t *cache)
    {
        auto params = reinterpret_cast<eviction::flexs_params_t *>(cache->eviction_params);
        while (params->main_limit < params->main->get_occupied_byte(params->main))
        {
            cache_obj_t *obj_to_evict = params->main->to_evict(params->main, NULL);
            int ori_freq = obj_to_evict->FLEX.freq;
            assert(ori_freq >= 0 && ori_freq <= MAXFREQ);
            copy_cache_obj_to_request(params->req_local, obj_to_evict);
            params->main->remove(params->main, obj_to_evict->obj_id);
            cache_obj_t *new_obj = params->sus->insert(params->sus, params->req_local);
            new_obj->FLEX.freq = ori_freq;
        }
        return;
    }

    static void evict_sus(cache_t *cache)
    {
        auto params = reinterpret_cast<eviction::flexs_params_t *>(cache->eviction_params);
        cache_obj_t *sus_to_evict = params->sus->to_evict(params->sus, NULL);
        int ori_freq = sus_to_evict->FLEX.freq;
        decreasepop(params->hotdistribution, ori_freq, -1);
        params->sus->remove(params->sus, sus_to_evict->obj_id);
    }

    static int compare(cache_t *cache){
        auto params = reinterpret_cast<eviction::flexs_params_t *>(cache->eviction_params);
        cache_obj_t *filter_to_evict = params->filter->to_evict(params->filter, NULL);
        cache_obj_t *sus_to_evict = params->sus->to_evict(params->sus, NULL);
        if(filter_to_evict==NULL||sus_to_evict==NULL){
            return 0;
        }
        if(sus_to_evict->FLEX.freq > 0){
            return 0;
        }
        int filter_value = minimalIncrementCBF_estimate(params->CBF, (void *)&filter_to_evict->obj_id,
                                                          sizeof(filter_to_evict->obj_id));
        if(filter_value > 0x3f){
            return 0;
        }
        int sus_value = minimalIncrementCBF_estimate(params->CBF, (void *)&sus_to_evict->obj_id,
                                                          sizeof(sus_to_evict->obj_id));
        if ( filter_value > sus_value){
            //add filter to sus and ghost (?)
            params->compareguard++;
            minimalIncrementCBF_add(params->CBF, (void *)&filter_to_evict->obj_id, sizeof(obj_id_t));
            copy_cache_obj_to_request(params->req_sus, filter_to_evict);
            cache_obj_t *new_obj = params->sus->insert(params->sus, params->req_sus);
            new_obj->FLEX.freq = 0;
            new_obj->FLEX.inghost = 1;
            params->hotdistribution[new_obj->FLEX.freq]++;
            cache_obj_t *new_obj_ghost = addtoghost(cache, params->req_sus, filter_to_evict->FLEX.freq);
            new_obj_ghost->FLEX.freq = filter_to_evict->FLEX.freq;
            params->filter->remove(params->filter, filter_to_evict->obj_id);
            return 1;
        }
        return 2;
    }

    static int adjust_sus(cache_t *cache){
        auto params = reinterpret_cast<eviction::flexs_params_t *>(cache->eviction_params);
        cache_obj_t *obj_to_evict = params->sus->to_evict(params->sus, NULL);
        if(obj_to_evict->FLEX.freq==0){
            return 1;
        }
        // move object from sus to main
        int ori_freq = obj_to_evict->FLEX.freq;
        copy_cache_obj_to_request(params->req_local, obj_to_evict);
        if(obj_to_evict->FLEX.inghost){
            params->evict_sus_ghost++;
            removefromghost(cache, params->req_local);
        }
        params->sus2main ++;
        params->sus->remove(params->sus, obj_to_evict->obj_id);
        // decreasepop(params->hotdistribution, ori_freq, 0);
        minimalIncrementCBF_add(params->CBF, (void *)&params->req_local->obj_id, sizeof(obj_id_t));
        cache_obj_t *new_obj = params->main->insert(params->main, params->req_local);
        new_obj->FLEX.freq = ori_freq-1;
        decreasepop(params->hotdistribution, ori_freq, ori_freq-1);
        return 0;
    }

    static int adjust_filter(cache_t *cache)
    {
        auto params = reinterpret_cast<eviction::flexs_params_t *>(cache->eviction_params);
        int has_evicted = 0;

        while (!has_evicted && params->filter->get_occupied_byte(params->filter) > 0)
        {
            cache_obj_t *obj_to_evict = params->filter->to_evict(params->filter, NULL);
            int ori_freq = obj_to_evict->FLEX.freq;
            copy_cache_obj_to_request(params->req_local, obj_to_evict);
            // add to cbf
            if (ori_freq >= params->guard_freq)
            {
                // move to main
                // decreasepop(params->hotdistribution, ori_freq, 0);
                params->filter2main++;
                cache_obj_t *new_obj = params->main->insert(params->main, params->req_local);
                new_obj->FLEX.freq = 0;
                decreasepop(params->hotdistribution, ori_freq, 0);
            }
            else
            {
                has_evicted = 1;
                int ret = compare(cache);
                if(ret == 1){
                    //filter wins compare, evict sus
                    //object movement done in compare
                    params->filter2sus++;
                    evict_sus(cache);
                    return has_evicted;
                }else{
                    //evict filter
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
        auto params = reinterpret_cast<eviction::flexs_params_t *>(cache->eviction_params);
        while (params->ghost->get_occupied_byte(params->ghost) > params->ghost_limit)
        {
            cache_obj_t *obj_to_evict = params->ghost->to_evict(params->ghost, NULL);
            int ori_freq = obj_to_evict->FLEX.freq;
            params->ghost->remove(params->ghost, obj_to_evict->obj_id);
            decreasepop(params->hotdistribution, ori_freq, -1);
        }
        return;
    }

    static void flexs_adjustguard(cache_t *cache)
    {
        auto params = reinterpret_cast<eviction::flexs_params_t *>(cache->eviction_params);
        // todo adjust guard_freq
        int guradfreq = params->guard_freq;
        // int guardthreshold = params->main_limit;
        int guardthreshold = params->main->get_n_obj(params->main);
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
        auto params = reinterpret_cast<eviction::flexs_params_t *>(cache->eviction_params);
        printf("filter: %ld sus: %ld main: %ld ghost: %ld\n",
               params->filter->get_occupied_byte(params->filter),
               params->sus->get_occupied_byte(params->sus),
               params->main->get_occupied_byte(params->main),
               params->ghost->get_occupied_byte(params->ghost));
        printf("phase %d gurad: %d ", params->phase, params->guard_freq);
        printf("hotdistribution: %d\n", params->guard_freq);
        for (auto &pop : params->hotdistribution)
        {
            printf("%d ", pop);
        }
        printf("\n");
        printf("filter_hitnum: %d sus_hitnum: %d main_hitnum: %d ghost_hitnum: %d\n",
               params->filter_hitnum, params->sus_hitnum, params->main_hitnum, params->ghost_hitnum);
        printf("ghost2main: %d ghost2sus: %d filter2sus: %d filter2main: %d filter2ghost: %d sus2main: %d evict_sus_ghost: %d\n",
               params->ghost2main, params->ghost2sus, params->filter2sus, params->filter2main, params->filter2ghost, params->sus2main, params->evict_sus_ghost);
        printf("compareguard: %d\n\n", params->compareguard);
        return;
    }

    static void flexs_evict(cache_t *cache, const request_t *req)
    {
        if (cache == nullptr || cache->eviction_params == nullptr)
        {
            fprintf(stderr, "Error: cache or cache->eviction_params is null\n");
            return;
        }

        auto params = reinterpret_cast<eviction::flexs_params_t *>(cache->eviction_params);
        params->evictobj_num += 1;
        if (params->evictobj_num == cache->get_n_obj(cache))
        {
            params->evictobj_num = 0;
            params->phase += 1;
            if ((params->phase & 0x1f) == 0)
            { // every 64 phase
                minimalIncrementCBF_decay(params->CBF);
                //printstatus(cache);
            }
        }

        flexs_adjustguard(cache);

        if(params->seq_miss > 10 && params->seq_miss % 10 == 0){
            adjust_sus(cache);
        }

        if(params->filter->get_occupied_byte(params->filter) + req->obj_size > params->filter_limit){
            //evict filter
            adjust_filter(cache);
        }else{
            //evict sus
            int ret = 0;
            while(ret == 0){
                adjust_main(cache);
                while (params->sus->get_occupied_byte(params->sus) > 0 && ret == 0)
                {
                    ret = adjust_sus(cache);
                }
            }
            // is compareing needed?
            compare(cache);
            evict_sus(cache);
            adjust_main(cache);
        }
        adjust_ghost(cache);
        return;
    }

    static bool flexs_remove(cache_t *cache, const obj_id_t obj_id)
    {
        auto params = reinterpret_cast<eviction::flexs_params_t *>(cache->eviction_params);
        bool removed = false;
        removed = removed || params->filter->remove(params->filter, obj_id);
        removed = removed || params->sus->remove(params->sus, obj_id);
        removed = removed || params->main->remove(params->main, obj_id);
        removed = removed || params->ghost->remove(params->ghost, obj_id);
        return removed;
        return true;
    }

    static inline int64_t flexs_get_occupied_byte(const cache_t *cache)
    {
        auto params = reinterpret_cast<eviction::flexs_params_t *>(cache->eviction_params);
        return params->filter->get_occupied_byte(params->filter) +
               params->sus->get_occupied_byte(params->sus) +
               params->main->get_occupied_byte(params->main);
        return cache_get_occupied_byte_default(cache);
    }

    static inline int64_t flexs_get_n_obj(const cache_t *cache)
    {
        auto params = reinterpret_cast<eviction::flexs_params_t *>(cache->eviction_params);
        return params->filter->get_n_obj(params->filter) +
               params->sus->get_n_obj(params->sus) +
               params->main->get_n_obj(params->main);
        return cache_get_n_obj_default(cache);
    }

    static inline bool flexs_can_insert(cache_t *cache, const request_t *req)
    {
        auto params = reinterpret_cast<eviction::flexs_params_t *>(cache->eviction_params);
        return req->obj_size <= params->filter->cache_size;
        return cache_can_insert_default(cache, req);
    }

    static void flexs_parse_params(cache_t *cache,
                                   const char *cache_specific_params)
    {
        auto *params = reinterpret_cast<eviction::flexs_params_t *>(cache->eviction_params);

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
            else if (strcasecmp(key, "suspected-size-ratio") == 0)
            {
                params->suspected_size_ratio = strtod(value, NULL);
            }
            else if (strcasecmp(key, "ghost-size-ratio") == 0)
            {
                params->ghost_size_ratio = strtod(value, NULL);
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
