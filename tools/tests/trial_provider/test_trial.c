#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "claw_endpoint.h"
#include "claw_http.h"
#include "trial_provider.h"
#include "trial_region.h"
#include "esp_app_desc.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

struct host_mutex { pthread_mutex_t mutex; };
struct host_event { pthread_mutex_t mutex; pthread_cond_t cond; EventBits_t bits; };
static atomic_int workers;
static atomic_int_fast64_t offset_us;
static atomic_int area = REGION_SERVICE_AREA_MAINLAND;
static atomic_int region_reads;
static atomic_int token_calls[2];
static atomic_bool gate[2], issuer_reject[2];

int64_t esp_timer_get_time(void)
{
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000 + atomic_load(&offset_us);
}
static struct timespec until(TickType_t ms)
{
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ms / 1000; ts.tv_nsec += (ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_nsec -= 1000000000L; ts.tv_sec++; }
    return ts;
}
SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    SemaphoreHandle_t out = calloc(1, sizeof(*out)); assert(out);
    pthread_mutex_init(&out->mutex, NULL); return out;
}
int xSemaphoreTake(SemaphoreHandle_t lock, TickType_t ticks)
{
    if (ticks == portMAX_DELAY) return pthread_mutex_lock(&lock->mutex) == 0;
    struct timespec end = until(ticks);
    return pthread_mutex_timedlock(&lock->mutex, &end) == 0;
}
int xSemaphoreGive(SemaphoreHandle_t lock) { return pthread_mutex_unlock(&lock->mutex) == 0; }
void vSemaphoreDelete(SemaphoreHandle_t lock) { pthread_mutex_destroy(&lock->mutex); free(lock); }
EventGroupHandle_t xEventGroupCreate(void)
{
    EventGroupHandle_t out = calloc(1, sizeof(*out)); assert(out);
    pthread_mutex_init(&out->mutex, NULL); pthread_cond_init(&out->cond, NULL); return out;
}
void vEventGroupDelete(EventGroupHandle_t e)
{ pthread_cond_destroy(&e->cond); pthread_mutex_destroy(&e->mutex); free(e); }
EventBits_t xEventGroupSetBits(EventGroupHandle_t e, EventBits_t bits)
{
    pthread_mutex_lock(&e->mutex); e->bits |= bits; EventBits_t out = e->bits;
    pthread_cond_broadcast(&e->cond); pthread_mutex_unlock(&e->mutex); return out;
}
EventBits_t xEventGroupClearBits(EventGroupHandle_t e, EventBits_t bits)
{
    pthread_mutex_lock(&e->mutex); EventBits_t out = e->bits; e->bits &= ~bits;
    pthread_mutex_unlock(&e->mutex); return out;
}
EventBits_t xEventGroupWaitBits(EventGroupHandle_t e, EventBits_t bits, int clear, int all, TickType_t ticks)
{
    struct timespec end = until(ticks);
    pthread_mutex_lock(&e->mutex);
    while (all ? (e->bits & bits) != bits : !(e->bits & bits)) {
        if (pthread_cond_timedwait(&e->cond, &e->mutex, &end)) break;
    }
    EventBits_t out = e->bits;
    if (clear) e->bits &= ~bits;
    pthread_mutex_unlock(&e->mutex); return out;
}
typedef struct { void (*fn)(void *); void *ctx; } host_job_t;
static void finished(void *unused) { atomic_fetch_sub(&workers, 1); }
static void *run_job(void *arg)
{
    host_job_t job = *(host_job_t *)arg; free(arg);
    pthread_cleanup_push(finished, NULL);
    job.fn(job.ctx);
    pthread_cleanup_pop(1);
    return NULL;
}
int xTaskCreate(void (*fn)(void *), const char *name, unsigned stack, void *ctx, unsigned priority, void *out)
{
    host_job_t *job = malloc(sizeof(*job)); assert(job); *job = (host_job_t){fn, ctx};
    pthread_t thread; atomic_fetch_add(&workers, 1);
    assert(pthread_create(&thread, NULL, run_job, job) == 0); pthread_detach(thread); return pdPASS;
}
void vTaskDelay(TickType_t ticks) { usleep(ticks * 1000); }
void vTaskDelete(void *task) { pthread_exit(NULL); }
const char *esp_err_to_name(esp_err_t err) { return "host-error"; }
esp_err_t esp_crt_bundle_attach(void *ctx) { return ESP_OK; }
const esp_app_desc_t *esp_app_get_description(void)
{ static const esp_app_desc_t desc = {.version = "test"}; return &desc; }
esp_err_t esp_read_mac(uint8_t *out, int type) { memset(out, 1, 6); return ESP_OK; }
esp_err_t region_service_get_status(region_service_handle_t handle, region_service_status_t *out)
{ memset(out, 0, sizeof(*out)); out->area = atomic_load(&area); atomic_fetch_add(&region_reads, 1); return ESP_OK; }
esp_err_t region_service_detect(region_service_handle_t handle, uint32_t timeout) { return ESP_OK; }
struct mock_http_client { char *url; int status; bool version; };
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *cfg)
{
    assert(cfg->is_async); esp_http_client_handle_t out = calloc(1, sizeof(*out)); assert(out);
    out->url = strdup(cfg->url); return out;
}
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c) { free(c->url); free(c); return ESP_OK; }
esp_err_t esp_http_client_set_header(esp_http_client_handle_t c, const char *name, const char *value)
{ if (!strcmp(name, "X-Firmware-Version")) c->version = !strcmp(value, "test"); return ESP_OK; }
int esp_http_client_get_status_code(esp_http_client_handle_t c) { return c->status; }
esp_err_t claw_http_send(esp_http_client_handle_t c, const char *body,
                         const claw_endpoint_request_t *request, claw_http_receive_fn receive, void *ctx)
{
    assert(c->version);
    int region = strstr(c->url, "cn.test") ? TRIAL_REGION_CN : TRIAL_REGION_INTL;
    if (atomic_load(&issuer_reject[region])) {
        c->status = 401; return receive("{}", 2, ctx);
    }
    c->status = 200;
    if (strstr(c->url, "/challenge")) {
        const char *response = "{\"challenge_id\":\"test\",\"nonce\":\"nonce\"}";
        return receive(response, strlen(response), ctx);
    }
    int generation = atomic_fetch_add(&token_calls[region], 1) + 1;
    while (atomic_load(&gate[region])) {
        esp_err_t err = claw_endpoint_request_check(request);
        if (err != ESP_OK) return err;
        usleep(1000);
    }
    char response[128];
    snprintf(response, sizeof(response), "{\"access_token\":\"region-%d-token-%d\",\"expires_in\":3600}", region, generation);
    return receive(response, strlen(response), ctx);
}
typedef struct {
    claw_endpoint_handle_t endpoint;
    atomic_bool cancel, done;
    claw_endpoint_resolved_t resolved;
    esp_err_t error;
} call_t;
static bool cancelled(void *ctx) { return atomic_load((atomic_bool *)ctx); }
static void *acquire(void *arg)
{
    call_t *call = arg;
    claw_endpoint_request_t request;
    claw_endpoint_request_init(&request, 2000, cancelled, &call->cancel);
    call->error = claw_endpoint_acquire(call->endpoint, &request, &call->resolved);
    atomic_store(&call->done, true); return NULL;
}
static void wait_value(atomic_int *value, int target)
{
    int64_t end = esp_timer_get_time() + 1000000;
    while (atomic_load(value) < target) { assert(esp_timer_get_time() < end); usleep(1000); }
}
static void wait_done(call_t *call)
{
    int64_t end = esp_timer_get_time() + 500000;
    while (!atomic_load(&call->done)) { assert(esp_timer_get_time() < end); usleep(1000); }
}
static claw_endpoint_handle_t endpoint(const char *service)
{
    claw_endpoint_handle_t out = NULL;
    assert(claw_endpoint_create(TRIAL_PROVIDER_ID, &(claw_endpoint_config_t){.service = service}, &out) == ESP_OK);
    return out;
}
static esp_err_t get(claw_endpoint_handle_t endpoint, uint32_t timeout, claw_endpoint_resolved_t *out)
{
    claw_endpoint_request_t request; claw_endpoint_request_init(&request, timeout, NULL, NULL);
    return claw_endpoint_acquire(endpoint, &request, out);
}
int main(void)
{
    int region_service;
    assert(trial_provider_init(&(trial_provider_config_t){.region_service = (void *)&region_service}) == ESP_OK);
    assert(trial_region_from_hint(" cn ") == TRIAL_REGION_CN);
    assert(trial_region_from_hint("INTL") == TRIAL_REGION_INTL);
    assert(trial_region_from_hint("unrecognized") == TRIAL_REGION_NONE);

    atomic_store(&gate[0], true);
    call_t first = {.endpoint = endpoint(CLAW_ENDPOINT_SERVICE_LLM)};
    call_t second = {.endpoint = endpoint(CLAW_ENDPOINT_SERVICE_ASR)};
    pthread_t t1, t2, t3;
    assert(!pthread_create(&t1, NULL, acquire, &first)); wait_value(&token_calls[0], 1);
    assert(!pthread_create(&t2, NULL, acquire, &second)); wait_value(&region_reads, 2);
    atomic_store(&first.cancel, true); wait_done(&first); pthread_join(t1, NULL);
    assert(first.error == CLAW_ENDPOINT_ERR_CANCELLED && !first.resolved.url);
    claw_endpoint_delete(first.endpoint);

    /* A different region must complete while mainland exchange is blocked. */
    atomic_store(&area, REGION_SERVICE_AREA_INTERNATIONAL);
    call_t international = {.endpoint = endpoint(CLAW_ENDPOINT_SERVICE_LLM)};
    assert(!pthread_create(&t3, NULL, acquire, &international)); wait_done(&international); pthread_join(t3, NULL);
    assert(international.error == ESP_OK);
    assert(!strcmp(international.resolved.url, "https://intl.test/v1"));
    assert(atomic_load(&token_calls[0]) == 1 && atomic_load(&token_calls[1]) == 1);
    atomic_store(&gate[0], false); wait_done(&second); pthread_join(t2, NULL);
    assert(second.error == ESP_OK);
    assert(!strcmp(second.resolved.url, "wss://cn.test/v1/asr/realtime"));
    assert(!strcmp(second.resolved.headers[0].value, "Bearer region-0-token-1"));
    assert(second.resolved.header_count == 2);

    atomic_store(&area, REGION_SERVICE_AREA_MAINLAND);
    claw_endpoint_handle_t llm = endpoint(CLAW_ENDPOINT_SERVICE_LLM);
    claw_endpoint_resolved_t old = {0}, fresh = {0}, cached = {0};
    assert(get(llm, 1000, &old) == ESP_OK);
    assert(old.credential_id == second.resolved.credential_id);
    assert(!claw_endpoint_report(llm, &old, CLAW_ENDPOINT_OUTCOME_ACCEPTED));
    assert(claw_endpoint_report(llm, &old, CLAW_ENDPOINT_OUTCOME_REJECTED));
    assert(get(llm, 1000, &fresh) == ESP_OK);
    assert(fresh.credential_id != old.credential_id);
    assert(claw_endpoint_report(llm, &old, CLAW_ENDPOINT_OUTCOME_REJECTED));
    assert(get(llm, 1000, &cached) == ESP_OK);
    assert(cached.credential_id == fresh.credential_id && atomic_load(&token_calls[0]) == 2);
    assert(!claw_endpoint_report(llm, &fresh, CLAW_ENDPOINT_OUTCOME_REJECTED));
    claw_endpoint_resolved_free(&cached);
    assert(get(llm, 1000, &cached) == CLAW_ENDPOINT_ERR_CREDENTIAL_REJECTED);
    assert(!cached.url && atomic_load(&token_calls[0]) == 2);

    atomic_store(&area, REGION_SERVICE_AREA_INTERNATIONAL);
    claw_endpoint_report(international.endpoint, &international.resolved, CLAW_ENDPOINT_OUTCOME_ACCEPTED);
    assert(claw_endpoint_report(international.endpoint, &international.resolved, CLAW_ENDPOINT_OUTCOME_REJECTED));
    atomic_store(&issuer_reject[1], true);
    assert(get(international.endpoint, 1000, &cached) == CLAW_ENDPOINT_ERR_CREDENTIAL_EXCHANGE);
    assert(get(international.endpoint, 1000, &cached) == CLAW_ENDPOINT_ERR_CREDENTIAL_EXCHANGE);

    /* Expire issuer backoff and exercise a timeout inside credential exchange. */
    atomic_fetch_add(&offset_us, 16000000);
    atomic_store(&issuer_reject[1], false);
    atomic_store(&gate[1], true);
    int64_t token_started = esp_timer_get_time();
    assert(get(international.endpoint, 30, &cached) == ESP_ERR_TIMEOUT);
    assert(esp_timer_get_time() - token_started < 200000);
    while (atomic_load(&workers)) usleep(1000);
    atomic_store(&gate[1], false);
    assert(get(international.endpoint, 1000, &cached) == ESP_OK);
    claw_endpoint_resolved_free(&cached);

    atomic_store(&area, REGION_SERVICE_AREA_UNKNOWN);
    int64_t started = esp_timer_get_time();
    assert(get(llm, 40, &cached) == ESP_ERR_TIMEOUT);
    assert(esp_timer_get_time() - started < 200000);
    claw_endpoint_resolved_free(&old); claw_endpoint_resolved_free(&fresh);
    claw_endpoint_resolved_free(&second.resolved); claw_endpoint_resolved_free(&international.resolved);
    claw_endpoint_delete(llm); claw_endpoint_delete(second.endpoint); claw_endpoint_delete(international.endpoint);
    while (atomic_load(&workers)) usleep(1000);
    puts("PASS: shared trial refresh, independent regions, cancelled waiter, URL/token snapshots, generations, rejection backoff, region deadline");
    return 0;
}
