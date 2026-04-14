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
        cache_t *front;
        cache_t *main;
        cache_t *small;
        cache_t *ghost;
        uint64_t front_limit;
        uint64_t main_limit;
        uint64_t small_limit;
        uint64_t ghost_limit;
        struct minimalIncrementCBF *CBF;
        //
        int32_t gurad_freq;
        std::vector<int32_t> popularity;
        int32_t phase;
        int32_t evictobj_num;
        int32_t ghost_freq;
        int32_t hit_on_ghost;
        int32_t move_to_main;
        int32_t prefetch;
        int32_t prefetch_freq;
        int32_t warmup;
        request_t *req_prefetch;
        request_t *req_small;
        //
        int32_t front_hitnum;
        int32_t small_hitnum;
        int32_t main_hitnum;
        int32_t ghost_hitnum;
        int32_t duelwin;

        int32_t ghost2main;
        int32_t ghost2small;
        int32_t front2small;
        int32_t front2main;
        int32_t front2ghost;
        int32_t small2main;
        int32_t evict_small_ghost;
        int32_t prefetch_num;
        int32_t prefetch_hitnum;

    } flexc_params_t;
} // namespace eviction

#ifdef __cplusplus
extern "C"
{
#endif

    cache_t *flexc_init(const common_cache_params_t ccache_params,
                        const char *cache_specific_params);
    static void flexc_free(cache_t *cache);
    static bool flexc_get(cache_t *cache, const request_t *req);

    static cache_obj_t *flexc_find(cache_t *cache, const request_t *req,
                                   const bool update_cache);
    static cache_obj_t *flexc_insert(cache_t *cache, const request_t *req);
    static cache_obj_t *flexc_to_evict(cache_t *cache, const request_t *req);
    static void flexc_evict(cache_t *cache, const request_t *req);
    static bool flexc_remove(cache_t *cache, const obj_id_t obj_id);
    static inline int64_t flexc_get_occupied_byte(const cache_t *cache);
    static inline int64_t flexc_get_n_obj(const cache_t *cache);
    static inline bool flexc_can_insert(cache_t *cache, const request_t *req);
    static void flexc_update(cache_t *cache);
    static void flexc_print(cache_t *cache);
    static void flexc_aging(cache_t *cache);

    cache_t *flexc_init(const common_cache_params_t ccache_params,
                        const char *cache_specific_params)
    {
        cache_t *cache = cache_struct_init("flexc", ccache_params, cache_specific_params);
        cache->eviction_params = reinterpret_cast<void *>(new eviction::flexc_params_t);

        cache->cache_init = flexc_init;
        cache->cache_free = flexc_free;
        cache->get = flexc_get;
        cache->find = flexc_find;
        cache->insert = flexc_insert;
        cache->evict = flexc_evict;
        cache->to_evict = flexc_to_evict;
        cache->remove = flexc_remove;
        cache->get_n_obj = flexc_get_n_obj;
        cache->get_occupied_byte = flexc_get_occupied_byte;
        cache->can_insert = flexc_can_insert;

        auto *params = reinterpret_cast<eviction::flexc_params_t *>(cache->eviction_params);

        params->req_local = new_request();
        params->req_prefetch = new_request();
        params->req_small = new_request();

        params->front_limit = MAX(1, (uint64_t)(0.1 * ccache_params.cache_size));
        params->small_limit = MAX(1, (uint64_t)(0.05 * ccache_params.cache_size));
        params->main_limit = ccache_params.cache_size - params->front_limit - params->small_limit;
        params->ghost_limit = ccache_params.cache_size;
        common_cache_params_t ccache_params_front = ccache_params;
        ccache_params_front.cache_size = params->front_limit;
        common_cache_params_t ccache_params_small = ccache_params;
        ccache_params_small.cache_size = params->small_limit;
        common_cache_params_t ccache_params_main = ccache_params;
        ccache_params_main.cache_size = params->main_limit;
        common_cache_params_t ccache_params_ghost = ccache_params;
        ccache_params_ghost.cache_size = params->ghost_limit;
        params->front = FIFO_init(ccache_params_front, cache_specific_params);
        params->small = FIFO_init(ccache_params_small, cache_specific_params);
        params->main = FIFO_init(ccache_params_main, cache_specific_params);
        params->ghost = FIFO_init(ccache_params_ghost, cache_specific_params);
        params->popularity = std::vector<int32_t>(MAXFREQ + 1, 0);
        params->phase = 0;
        params->evictobj_num = 0;
        params->timer = 0;
        params->gurad_freq = 2;
        params->CBF =
            (struct minimalIncrementCBF *)malloc(sizeof(struct minimalIncrementCBF));
        params->CBF->ready = 0;
        int ret = minimalIncrementCBF_init(params->CBF,
                                           ccache_params.cache_size * 4, 0.001);
        if (ret != 0)
        {
            ERROR("CBF init failed\n");
        }
        params->hit_on_ghost = 0;
        params->move_to_main = 0;
        params->front_hitnum = 0;
        params->small_hitnum = 0;
        params->main_hitnum = 0;
        params->ghost_hitnum = 0;

        params->ghost2main = 0;
        params->ghost2small = 0;
        params->front2small = 0;
        params->front2main = 0;
        params->front2ghost = 0;
        params->small2main = 0;
        params->evict_small_ghost = 0;
        params->prefetch_num = 0;
        params->prefetch_hitnum = 0;
        params->prefetch = 0;
        return cache;
    }

    static void flexc_free(cache_t *cache)
    {
        auto *params = reinterpret_cast<eviction::flexc_params_t *>(cache->eviction_params);
        params->front->cache_free(params->front);
        params->small->cache_free(params->small);
        params->main->cache_free(params->main);
        params->ghost->cache_free(params->ghost);
        minimalIncrementCBF_free(params->CBF);
        free(params->CBF);
        free_request(params->req_local);
        free_request(params->req_prefetch);
        delete reinterpret_cast<eviction::flexc_params_t *>(cache->eviction_params);
        cache_struct_free(cache);
    }

    static bool flexc_get(cache_t *cache, const request_t *req)
    {
        return cache_get_base(cache, req);
    }
static void printstatus(cache_t *cache);
    static cache_obj_t *addtoghost(cache_t *cache, const request_t *req, int freq);
    static cache_obj_t *flexc_prefetch(cache_t *cache, cache_obj_t *obj_toprefetch)
    {
        auto *params = reinterpret_cast<eviction::flexc_params_t *>(cache->eviction_params);
        if (obj_toprefetch->FLEX.prefetch != 0)
        {
            return NULL;
        }
        obj_toprefetch->FLEX.prefetch = 1;
        copy_cache_obj_to_request(params->req_prefetch, obj_toprefetch);
        params->prefetch_num++;
        params->prefetch = 1;
        params->prefetch_freq = obj_toprefetch->FLEX.freq;
        return obj_toprefetch;
    }
    static cache_obj_t *flexc_prefetchinsert(cache_t *cache){
        auto *params = reinterpret_cast<eviction::flexc_params_t *>(cache->eviction_params);
        cache_obj_t *obj = params->front->insert(params->front, params->req_prefetch);
        obj->FLEX.freq = params->prefetch_freq;
        obj->FLEX.prefetch = 1;
        params->prefetch = 0;
        return obj;
    }

    static cache_obj_t *flexc_find(cache_t *cache, const request_t *req,
                                   const bool update_cache)
    {
        auto *params = reinterpret_cast<eviction::flexc_params_t *>(cache->eviction_params);
        cache_obj_t *obj = NULL;
        if (!update_cache)
        {
            obj = params->front->find(params->front, req, update_cache);
            if (obj != NULL)
            {
                return obj;
            }
            obj = params->small->find(params->small, req, update_cache);
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
        if(params->timer%1000000==0){
            //printstatus(cache);
        }
        obj = params->front->find(params->front, req, update_cache);
        if (obj != NULL)
        {
            if (obj->FLEX.prefetch==1&&obj->FLEX.prefetchhit==0)
            {
                params->prefetch_hitnum++;
                obj->FLEX.prefetchhit = 1;
                cache_obj_t *ghost_obj = params->ghost->find(params->ghost, req, false);
                if (ghost_obj != NULL)
                {
                    // prefetch hit prefetch next

                    cache_obj_t *pobj = ghost_obj->queue.prev;
                    if (pobj != NULL && pobj->FLEX.prefetch == 0)
                    {

                        flexc_prefetch(cache, pobj);
                        flexc_prefetchinsert(cache);
                    }
                    // todo? remove from ghost
                    if (ghost_obj->FLEX.prefetchhit)
                    {
                        // hit twice
                        obj->FLEX.prefetch = 0;
                        obj->FLEX.prefetchhit = 0;
                        if (obj->FLEX.freq < MAXFREQ)
                        {
                            obj->FLEX.freq++;
                            params->popularity[obj->FLEX.freq] += 1;
                        }
                    }
                    params->ghost->remove(params->ghost, ghost_obj->obj_id);
                }else{
                    for(int i = 0; i <= obj->FLEX.freq; i++){
                        params->popularity[i]++;
                    }
                }
            }
            else
            {
                params->front_hitnum++;
                if (obj->FLEX.freq < MAXFREQ)
                {
                    obj->FLEX.freq++;
                    params->popularity[obj->FLEX.freq] += 1;
                }
            }

            return obj;
        }
        obj = params->small->find(params->small, req, update_cache);
        if (obj != NULL)
        {
            params->small_hitnum++;
            obj->FLEX.sushit = 1;
            if (obj->FLEX.freq < MAXFREQ)
            {
                obj->FLEX.freq++;
                params->popularity[obj->FLEX.freq] += 1;
            }
            return obj;
        }
        obj = params->main->find(params->main, req, update_cache);
        if (obj != NULL)
        {
            params->main_hitnum++;
            if (obj->FLEX.freq < MAXFREQ)
            {
                obj->FLEX.freq++;
                params->popularity[obj->FLEX.freq] += 1;
            }
            return obj;
        }
        obj = params->ghost->find(params->ghost, req, false);
        if (obj != NULL)
        {
            params->ghost_hitnum++;
            if (obj->FLEX.freq < MAXFREQ)
            {
                obj->FLEX.freq++;
                params->popularity[obj->FLEX.freq] += 1;
            }
            params->hit_on_ghost = true;
            params->ghost_freq = obj->FLEX.freq;
            if (obj->FLEX.freq >= params->gurad_freq)
            { // important in fiu
                params->move_to_main = true;
            }
            cache_obj_t *seqobj = obj->queue.prev;
            if (seqobj != NULL)
            {
                seqobj->FLEX.seqaccess = 1;
                if (obj->FLEX.seqaccess == 1&&seqobj->FLEX.prefetch==0)
                {
                    flexc_prefetch(cache, seqobj);
                }
            }
        }
        return NULL;
        return obj;
    }
    
    static void decreasepop(std::vector<int32_t> &popularity, int32_t ori_freq, int32_t dest_freq)
    {
        for (int i = ori_freq; i > dest_freq; i--)
        {
            popularity[i]--;
        }
    }

    static cache_obj_t *flexc_insert(cache_t *cache, const request_t *req)
    {
        auto *params = reinterpret_cast<eviction::flexc_params_t *>(cache->eviction_params);
        cache_obj_t *obj = NULL;
        // if(params->main->get_occupied_byte(params->main) < params->main_limit){
        if (params->small->get_occupied_byte(params->small) == 0 && params->front->get_occupied_byte(params->front) == 0)
        {
            // warmup
            obj = params->main->insert(params->main, req);
            obj->FLEX.freq = 0;
            params->popularity[0]++;
            // addtoghost(cache, req);
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
                    obj->FLEX.freq=params->ghost_freq;
                    params->ghost->remove(params->ghost, obj->obj_id);
                }
                else
                {
                    params->ghost2small++;
                    //minimalIncrementCBF_add(params->CBF, (void *)&req->obj_id, sizeof(obj_id_t));
                    obj = params->small->insert(params->small, req);
                    obj->FLEX.freq = 0;
                    //decreasepop(params->popularity, obj->FLEX.freq, 0);
                    //?? inghost?
                    obj->FLEX.inghost = 1;
                    params->popularity[0]++;
                }
                //obj->FLEX.freq = params->ghost_freq;
                
                
                if(params->prefetch){
                    flexc_prefetchinsert(cache);
                }
            }
            else
            {
                // new obj
                obj = params->front->insert(params->front, req);
                obj->FLEX.freq = 0;
                params->popularity[0]++;
            }
        }
        return obj;
    }

    static cache_obj_t *flexc_to_evict(cache_t *cache, const request_t *req)
    {
        auto params = reinterpret_cast<eviction::flexc_params_t *>(cache->eviction_params);
        assert(0);
        return NULL;
    }



    static cache_obj_t *addtoghost(cache_t *cache, const request_t *req, int freq)
    {
        auto params = reinterpret_cast<eviction::flexc_params_t *>(cache->eviction_params);
        cache_obj_t *object = params->ghost->insert(params->ghost, req);
        object->FLEX.freq = freq;
        object->FLEX.seqaccess = 0;
        return object;
    }

    static int movefromghost(cache_t *cache, const request_t *req)
    {
        auto params = reinterpret_cast<eviction::flexc_params_t *>(cache->eviction_params);
        cache_obj_t *obj = params->ghost->find(params->ghost, req, false);
        if (obj != NULL)
        {
            int ori_freq = obj->FLEX.freq;
            decreasepop(params->popularity, ori_freq, -1);
            params->ghost->remove(params->ghost, obj->obj_id);
            return ori_freq;
        }
        return 0;
    }

    static void adjust_main(cache_t *cache)
    {
        auto params = reinterpret_cast<eviction::flexc_params_t *>(cache->eviction_params);
        while(params->main_limit < params->main->get_occupied_byte(params->main)){
            cache_obj_t *obj_to_evict = params->main->to_evict(params->main, NULL);
            int ori_freq = obj_to_evict->FLEX.freq;
            assert(ori_freq >= 0 && ori_freq <= MAXFREQ);
            copy_cache_obj_to_request(params->req_local, obj_to_evict);
            params->main->remove(params->main, obj_to_evict->obj_id);
            cache_obj_t *new_obj = params->small->insert(params->small, params->req_local);
            new_obj->FLEX.freq = ori_freq;
            /*
            if(ori_freq==0){
                break;
            }
            */
        }
        return;
    }

    static void evict_small(cache_t *cache)
    {
        auto params = reinterpret_cast<eviction::flexc_params_t *>(cache->eviction_params);
        cache_obj_t *small_to_evict = params->small->to_evict(params->small, NULL);
        int ori_freq = small_to_evict->FLEX.freq;
        decreasepop(params->popularity, ori_freq, -1);
        //copy_cache_obj_to_request(params->req_small, small_to_evict);
        //cache_obj_t *new_obj = addtoghost(cache, params->req_small, small_to_evict->FLEX.freq);
        //new_obj->FLEX.freq = small_to_evict->FLEX.freq;
        params->small->remove(params->small, small_to_evict->obj_id);
    }

    static int adjust_small_old(cache_t *cache){
        auto params = reinterpret_cast<eviction::flexc_params_t *>(cache->eviction_params);
        while (params->small->get_occupied_byte(params->small) > 0)
        {
            cache_obj_t *obj_to_evict = params->small->to_evict(params->small, NULL);
            if (obj_to_evict->FLEX.sushit == 0)
            {
                break;
            }
            /*
            if(obj_to_evict->FLEX.freq==0){
                break;
            }
            */
            // move object from small to main
            int ori_freq = obj_to_evict->FLEX.freq;
            copy_cache_obj_to_request(params->req_local, obj_to_evict);
            params->small->remove(params->small, obj_to_evict->obj_id);
            // decreasepop(params->popularity, ori_freq, 0);
            //minimalIncrementCBF_add(params->CBF, (void *)&params->req_local->obj_id, sizeof(obj_id_t));
            cache_obj_t *new_obj = params->main->insert(params->main, params->req_local);
            new_obj->FLEX.freq = 0;
            decreasepop(params->popularity, ori_freq, 0);
        }
        if(params->small->get_occupied_byte(params->small) > params->small_limit){
            evict_small(cache);
            return 1;
        }
        return 0;
    }

    static int duel(cache_t *cache){
        return 0;
        auto params = reinterpret_cast<eviction::flexc_params_t *>(cache->eviction_params);
        cache_obj_t *front_to_evict = params->front->to_evict(params->front, NULL);
        cache_obj_t *small_to_evict = params->small->to_evict(params->small, NULL);
        if(front_to_evict==NULL||small_to_evict==NULL){
            return 0;
        }
        if(front_to_evict->FLEX.prefetch == 1||front_to_evict->FLEX.freq > params->gurad_freq){
            return 0;
        }
        if(small_to_evict->FLEX.freq > 0){
            return 0;
        }
        int front_value = minimalIncrementCBF_estimate(params->CBF, (void *)&front_to_evict->obj_id,
                                                          sizeof(front_to_evict->obj_id));
        if(front_value > 0x3f){
            return 0;
        }
        int small_value = minimalIncrementCBF_estimate(params->CBF, (void *)&small_to_evict->obj_id,
                                                          sizeof(small_to_evict->obj_id));
        if ( front_value > small_value){
            //add front to small and ghost (?)
            params->duelwin++;
            minimalIncrementCBF_add(params->CBF, (void *)&front_to_evict->obj_id, sizeof(obj_id_t));
            copy_cache_obj_to_request(params->req_small, front_to_evict);
            cache_obj_t *new_obj = params->small->insert(params->small, params->req_small);
            new_obj->FLEX.freq = 0;
            new_obj->FLEX.inghost = 1;
            params->popularity[new_obj->FLEX.freq]++;
            cache_obj_t *new_obj_ghost = addtoghost(cache, params->req_small, front_to_evict->FLEX.freq);
            new_obj_ghost->FLEX.freq = front_to_evict->FLEX.freq;
            params->front->remove(params->front, front_to_evict->obj_id);
            return 1;
        }
        return 2;
    }

    static int adjust_small(cache_t *cache){
        auto params = reinterpret_cast<eviction::flexc_params_t *>(cache->eviction_params);
        while (params->small->get_occupied_byte(params->small) > 0)
        {
            cache_obj_t *obj_to_evict = params->small->to_evict(params->small, NULL);
            if(obj_to_evict->FLEX.freq==0){
                return 1;
                break;
            }
            // move object from small to main
            int ori_freq = obj_to_evict->FLEX.freq;
            copy_cache_obj_to_request(params->req_local, obj_to_evict);
            if(obj_to_evict->FLEX.inghost){
                params->evict_small_ghost++;
                movefromghost(cache, params->req_local);
            }
            params->small2main ++;
            params->small->remove(params->small, obj_to_evict->obj_id);
            // decreasepop(params->popularity, ori_freq, 0);
            minimalIncrementCBF_add(params->CBF, (void *)&params->req_local->obj_id, sizeof(obj_id_t));
            cache_obj_t *new_obj = params->main->insert(params->main, params->req_local);
            new_obj->FLEX.freq = ori_freq-1;
            decreasepop(params->popularity, ori_freq, ori_freq-1);
        }
        return 0;
    }

    static int adjust_front(cache_t *cache)
    {
        auto params = reinterpret_cast<eviction::flexc_params_t *>(cache->eviction_params);
        int has_evicted = 0;

        while (!has_evicted && params->front->get_occupied_byte(params->front) > 0)
        {
            cache_obj_t *obj_to_evict = params->front->to_evict(params->front, NULL);

            int ori_freq = obj_to_evict->FLEX.freq;
            int prefetchhit = obj_to_evict->FLEX.prefetchhit;

            if (obj_to_evict->FLEX.prefetch == 1 && obj_to_evict->FLEX.prefetchhit == 0)
            {
                // prefetch not hit discard only
                // decreasepop(params->popularity, ori_freq, -1);
                params->front->remove(params->front, obj_to_evict->obj_id);
                has_evicted = 1;
                continue;
            }
            copy_cache_obj_to_request(params->req_local, obj_to_evict);
            // add to cbf

            if (ori_freq >= params->gurad_freq)
            {
                // move to main
                // decreasepop(params->popularity, ori_freq, 0);
                params->front2main++;
                cache_obj_t *new_obj = params->main->insert(params->main, params->req_local);
                new_obj->FLEX.freq = 0;
                decreasepop(params->popularity, ori_freq, 0);
            }
            else
            {
                has_evicted = 1;
                int ret = duel(cache);
                if(ret == 1){
                    //win duel evict small
                    params->front2small++;
                    evict_small(cache);
                    return has_evicted;
                }else{
                    //evict front
                    params->front2ghost++;
                    cache_obj_t *new_obj = addtoghost(cache, params->req_local, ori_freq);
                    //new_obj->FLEX.freq = ori_freq;
                    new_obj->FLEX.prefetchhit = prefetchhit;
                }

            }
            //minimalIncrementCBF_add(params->CBF, (void *)&params->req_local->obj_id, sizeof(obj_id_t));
            params->front->remove(params->front, obj_to_evict->obj_id);
        }

        return has_evicted;
    }

    static void adjust_ghost(cache_t *cache)
    {
        auto params = reinterpret_cast<eviction::flexc_params_t *>(cache->eviction_params);
        while (params->ghost->get_occupied_byte(params->ghost) > params->ghost_limit)
        {
            cache_obj_t *obj_to_evict = params->ghost->to_evict(params->ghost, NULL);
            int ori_freq = obj_to_evict->FLEX.freq;
            params->ghost->remove(params->ghost, obj_to_evict->obj_id);
            decreasepop(params->popularity, ori_freq, -1);
        }
        return;
    }

    static void flexc_adjustguard(cache_t *cache)
    {
        auto params = reinterpret_cast<eviction::flexc_params_t *>(cache->eviction_params);
        // todo adjust gurad_freq
        int guradfreq = params->gurad_freq;
        int guardthreshold = params->main_limit;
        if (params->popularity[guradfreq] > guardthreshold)
        {
            while (params->popularity[guradfreq] > guardthreshold)
            {
                guradfreq++;
            }
        }
        else
        {
            while (guradfreq > 1 && params->popularity[guradfreq - 1] < guardthreshold)
            {
                guradfreq--;
            }
        }
        params->gurad_freq = guradfreq;
        return;
    }

    static void printstatus(cache_t *cache)
    {
        auto params = reinterpret_cast<eviction::flexc_params_t *>(cache->eviction_params);
        printf("front: %ld small: %ld main: %ld ghost: %ld\n",
               params->front->get_occupied_byte(params->front),
               params->small->get_occupied_byte(params->small),
               params->main->get_occupied_byte(params->main),
               params->ghost->get_occupied_byte(params->ghost));
        printf("phase %d gurad: %d ", params->phase, params->gurad_freq);
        printf("popularity: %d\n", params->gurad_freq);
        for (auto &pop : params->popularity)
        {
            printf("%d ", pop);
        }
        printf("\n");
        printf("front_hitnum: %d small_hitnum: %d main_hitnum: %d ghost_hitnum: %d\n",
               params->front_hitnum, params->small_hitnum, params->main_hitnum, params->ghost_hitnum);
        printf("ghost2main: %d ghost2small: %d front2small: %d front2main: %d front2ghost: %d small2main: %d evict_small_ghost: %d\n",
               params->ghost2main, params->ghost2small, params->front2small, params->front2main, params->front2ghost, params->small2main, params->evict_small_ghost);
        printf("prefetch_num: %d prefetch_hitnum: %d\n", params->prefetch_num, params->prefetch_hitnum);
        printf("duelwin: %d\n\n", params->duelwin);
        return;
    }

    static void flexc_evict(cache_t *cache, const request_t *req)
    {
        if (cache == nullptr || cache->eviction_params == nullptr)
        {
            fprintf(stderr, "Error: cache or cache->eviction_params is null\n");
            return;
        }

        auto params = reinterpret_cast<eviction::flexc_params_t *>(cache->eviction_params);
        params->evictobj_num += 1;

        if (params->evictobj_num == cache->cache_size)
        {
            params->evictobj_num = 0;
            params->phase += 1;

            if ((params->phase & 0x1f) == 0)
            { // every 64 phase
                //minimalIncrementCBF_decay(params->CBF);
                //printstatus(cache);
            }

        }

        flexc_adjustguard(cache);

        if(params->front->get_occupied_byte(params->front) > params->front_limit){
            //evict front
            adjust_front(cache);
        }else{
            //evict small
            int ret = 0;
            while(ret == 0){
                adjust_main(cache);
                ret = adjust_small(cache);
            }
            duel(cache);
            evict_small(cache);
        }
        adjust_ghost(cache);
        return;

        int ret = adjust_small(cache);
        if(ret){
            adjust_ghost(cache);
            return;
        }
        adjust_main(cache);
        adjust_front(cache);
        adjust_ghost(cache);

        return;
    }

    static bool flexc_remove(cache_t *cache, const obj_id_t obj_id)
    {
        auto params = reinterpret_cast<eviction::flexc_params_t *>(cache->eviction_params);
        bool removed = false;
        removed = removed || params->front->remove(params->front, obj_id);
        removed = removed || params->small->remove(params->small, obj_id);
        removed = removed || params->main->remove(params->main, obj_id);
        removed = removed || params->ghost->remove(params->ghost, obj_id);
        return removed;
        return true;
    }

    static inline int64_t flexc_get_occupied_byte(const cache_t *cache)
    {
        auto params = reinterpret_cast<eviction::flexc_params_t *>(cache->eviction_params);
        return params->front->get_occupied_byte(params->front) +
               params->small->get_occupied_byte(params->small) +
               params->main->get_occupied_byte(params->main);
        return cache_get_occupied_byte_default(cache);
    }

    static inline int64_t flexc_get_n_obj(const cache_t *cache)
    {
        auto params = reinterpret_cast<eviction::flexc_params_t *>(cache->eviction_params);
        return params->front->get_n_obj(params->front) +
               params->small->get_n_obj(params->small) +
               params->main->get_n_obj(params->main);
        return cache_get_n_obj_default(cache);
    }

    static inline bool flexc_can_insert(cache_t *cache, const request_t *req)
    {
        auto params = reinterpret_cast<eviction::flexc_params_t *>(cache->eviction_params);
        return req->obj_size <= params->front->cache_size;
        return cache_can_insert_default(cache, req);
    }

#ifdef __cplusplus
}
#endif
