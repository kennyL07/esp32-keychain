/* Local WPA2 AP protocol v2. Uploads remain hidden until validated and committed. */
#include "storage_api.h"
#include "board.h"
#include "player.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_random.h"
#include "esp_vfs_fat.h"
#include "esp_log.h"
#include "freertos/task.h"

#define PATH_SIZE 224
#define MAX_FILE (256UL * 1024 * 1024)
static char upload_id[65], upload_token[17], staging[PATH_SIZE];
/* HTTP handlers run serially. Never place this buffer on the HTTP task stack. */
static unsigned char upload_buffer[4096];
static bool upload_holds_player;
static const char *files[] = {"loop.epv", "intro.epv", "loop-clean.epv", "intro-clean.epv", "metadata.json", "preview.jpg"};
#define FILE_COUNT (sizeof(files) / sizeof(files[0]))

static esp_err_t fail(httpd_req_t *req, const char *status, const char *message)
{
    httpd_resp_set_status(req, status);
    return httpd_resp_sendstr(req, message);
}
bool storage_api_valid_id(const char *s)
{
    size_t n = strlen(s);
    if (!n || n > 64) return false;
    for (size_t i = 0; i < n; ++i)
        if (!(s[i]>='A'&&s[i]<='Z') && !(s[i]>='a'&&s[i]<='z') && !(s[i]>='0'&&s[i]<='9') && s[i] != '-' && s[i] != '_') return false;
    return true;
}
static bool param(httpd_req_t *req, const char *key, char *out, size_t size)
{
    char query[384];
    return httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, key, out, size) == ESP_OK;
}
static bool number(httpd_req_t *req, const char *key, unsigned long *result)
{
    char value[24], *end;
    if (!param(req, key, value, sizeof(value)) || !*value || value[0] == '-') return false;
    errno = 0;
    *result = strtoul(value, &end, 10);
    return !*end && !errno;
}
static void remove_material(const char *path)
{
    char file[PATH_SIZE + 32];
    for (unsigned i = 0; i < FILE_COUNT; ++i) {
        snprintf(file, sizeof(file), "%s/%s", path, files[i]);
        unlink(file);
    }
    rmdir(path);
}
void storage_api_cleanup(void)
{
    if (*staging) remove_material(staging);
    staging[0] = upload_id[0] = upload_token[0] = 0;
    if (upload_holds_player) { upload_holds_player = false; player_storage_end(); }
}
static bool session(httpd_req_t *req)
{
    char token[24];
    return *upload_token && param(req, "token", token, sizeof(token)) && !strcmp(token, upload_token);
}
static esp_err_t materials(httpd_req_t *req)
{
    player_material_index_t *index = malloc(sizeof(*index));
    if (!index) return fail(req, "503 Service Unavailable", "Not enough memory.");
    player_materials_snapshot(index);
    char buf[256];
    snprintf(buf, sizeof(buf), "{\"free_bytes\":%llu,\"material_revision\":%u,\"materials\":[", (unsigned long long)index->free_bytes, index->revision);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr_chunk(req, buf);
    for (unsigned i = 0; i < index->count && err == ESP_OK; ++i) {
        snprintf(buf, sizeof(buf), "%s{\"id\":\"%s\",\"index\":%u}", i ? "," : "", index->ids[i], i);
        err = httpd_resp_sendstr_chunk(req, buf);
    }
    free(index);
    if (err != ESP_OK) return err;
    httpd_resp_sendstr_chunk(req, "]}");
    return httpd_resp_send_chunk(req, NULL, 0);
}
static esp_err_t start(httpd_req_t *req)
{
    char id[65], dest[PATH_SIZE]; struct stat info;
    unsigned long bytes;
    if (!player_status().storage_ready) return fail(req, "503 Service Unavailable", "Insert a FAT32 SD card and restart.");
    if (!param(req, "id", id, sizeof(id)) || !storage_api_valid_id(id) || !number(req, "bytes", &bytes) || !bytes)
        return fail(req, "400 Bad Request", "Invalid material ID or size.");
    mkdir(CARD_ROOT, 0775);
    snprintf(dest, sizeof(dest), "%s/%s", CARD_ROOT, id);
    if (!stat(dest, &info)) return fail(req, "409 Conflict", "This material already exists. Delete it before replacing it.");
    if (player_status().card_count >= 128) return fail(req, "409 Conflict", "The 128-material limit has been reached.");
    uint64_t total, free_bytes;
    if (esp_vfs_fat_info("/sdcard", &total, &free_bytes) != ESP_OK || free_bytes < (uint64_t)bytes + 1024 * 1024)
        return fail(req, "507 Insufficient Storage", "Not enough free space on the SD card.");
    storage_api_cleanup();
    if (!player_storage_begin()) return fail(req, "503 Service Unavailable", "Player is busy. Retry upload.");
    upload_holds_player = true;
    char transport[8];
    strcpy(upload_id, id);
    snprintf(staging, sizeof(staging), "%s/.incoming_%s", CARD_ROOT, id);
    remove_material(staging); /* Recover an incomplete upload left by a reset. */
    if (mkdir(staging, 0775)) { storage_api_cleanup(); return fail(req, "500 Internal Server Error", "Cannot create upload folder."); }
    snprintf(upload_token, sizeof(upload_token), "%08lx%08lx", (unsigned long)esp_random(), (unsigned long)esp_random());
    char reply[64]; snprintf(reply, sizeof(reply), "{\"token\":\"%s\"}", upload_token);
    httpd_resp_set_type(req, "application/json");
    ESP_LOGI("upload", "Started %s; playback suspended; HTTP stack remaining: %u bytes",
             upload_id, (unsigned)uxTaskGetStackHighWaterMark(NULL));
    return httpd_resp_sendstr(req, reply);
}
static esp_err_t chunk(httpd_req_t *req)
{
    char name[24], path[PATH_SIZE + 32]; unsigned long offset;
    if (!session(req)) return fail(req, "409 Conflict", "Upload session expired. Start again.");
    if (!param(req, "file", name, sizeof(name)) || !number(req, "offset", &offset))
        return fail(req, "400 Bad Request", "Missing file or offset.");
    bool known = false;
    for (unsigned i = 0; i < FILE_COUNT; ++i) if (!strcmp(name, files[i])) known = true;
    if (!known || req->content_len < 1 || req->content_len > 128 * 1024 || offset > MAX_FILE ||
        req->content_len > MAX_FILE - offset)
        return fail(req, "400 Bad Request", "Invalid upload file or chunk size.");
    if ((!strcmp(name, "metadata.json") || !strcmp(name, "preview.jpg")) && offset + req->content_len > 1024 * 1024)
        return fail(req, "400 Bad Request", "Metadata and preview are limited to 1 MiB each.");
    snprintf(path, sizeof(path), "%s/%s", staging, name);
    struct stat info; size_t existing = !stat(path, &info) ? info.st_size : 0;
    if (offset > existing) return fail(req, "409 Conflict", "Chunk offset is ahead of the stored file.");
    FILE *out = fopen(path, existing ? "r+b" : "w+b");
    if (!out) return fail(req, "500 Internal Server Error", "Cannot open staged file.");
    /* Retrying the same offset overwrites a partial chunk after a disconnect. */
    if (fseek(out, offset, SEEK_SET)) { fclose(out); return ESP_FAIL; }
    size_t remaining = req->content_len, processed = 0;
    while (remaining) {
        int n = httpd_req_recv(req, (char *)upload_buffer, remaining < sizeof(upload_buffer) ? remaining : sizeof(upload_buffer));
        if (n <= 0 || fwrite(upload_buffer, 1, n, out) != n) {
            fclose(out);
            return fail(req, "500 Internal Server Error", "Upload interrupted. Retry this chunk.");
        }
        remaining -= n;
        processed += n;
        if (processed >= 16384) { vTaskDelay(1); processed = 0; }
    }
    bool ok = !fflush(out) && !ftruncate(fileno(out), offset + req->content_len);
    if (fclose(out)) ok = false;
    return ok ? httpd_resp_sendstr(req, "OK") : fail(req, "500 Internal Server Error", "SD write failed.");
}
static unsigned le16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static uint32_t le32(const unsigned char *p) { return (uint32_t)p[0] | ((uint32_t)p[1]<<8) | ((uint32_t)p[2]<<16) | ((uint32_t)p[3]<<24); }
static bool valid_epv(const char *name)
{
    char path[PATH_SIZE + 32]; snprintf(path, sizeof(path), "%s/%s", staging, name);
    FILE *in = fopen(path, "rb");
    if (!in) return false;
    unsigned char h[24], n[4], marker[2]; bool ok = false;
    if (fread(h, 1, 24, in) != 24 || memcmp(h,"EPV1",4) || le16(h+4)!=320 || le16(h+6)!=820 ||
        !le16(h+8) || le16(h+8)>30 || !le16(h+10) || le16(h+10)>1000 ||
        !le32(h+12) || le32(h+12)>216000 || !le32(h+16) || le32(h+16)>1048576 || le32(h+20)) goto done;
    for (uint32_t i=0; i<le32(h+12); ++i) {
        if (fread(n,1,4,in)!=4) goto done;
        uint32_t size=le32(n);
        if (size<4 || size>le32(h+16) || fread(marker,1,2,in)!=2 || marker[0]!=255 || marker[1]!=216 ||
            fseek(in,size-4,SEEK_CUR) || fread(marker,1,2,in)!=2 || marker[0]!=255 || marker[1]!=217) goto done;
        if (!(i % 64)) vTaskDelay(1);
    }
    ok = fgetc(in)==EOF && !ferror(in);
done:
    fclose(in); return ok;
}
static esp_err_t commit(httpd_req_t *req)
{
    if (!session(req)) return fail(req,"409 Conflict","Upload session expired.");
    char optional[PATH_SIZE+32]; struct stat info;
    snprintf(optional,sizeof(optional),"%s/intro.epv",staging);
    if (!valid_epv("loop.epv") || (!stat(optional,&info) && !valid_epv("intro.epv")))
        return fail(req,"400 Bad Request","Invalid or incomplete EPV video. Upload a converted 320 x 820 material.");
    bool has_intro = stat(optional, &info) == 0;
    snprintf(optional,sizeof(optional),"%s/loop-clean.epv",staging);
    bool has_clean = stat(optional, &info) == 0;
    snprintf(optional,sizeof(optional),"%s/intro-clean.epv",staging);
    bool has_clean_intro = stat(optional, &info) == 0;
    if ((has_clean_intro && !has_clean) || (has_clean && has_intro && !has_clean_intro) ||
        (has_clean && !valid_epv("loop-clean.epv")) || (has_clean_intro && !valid_epv("intro-clean.epv")))
        return fail(req,"400 Bad Request","Overlay toggle streams are incomplete or invalid.");
    if (!upload_holds_player) return fail(req,"409 Conflict","Upload session expired. Start again.");
    char dest[PATH_SIZE]; snprintf(dest,sizeof(dest),"%s/%s",CARD_ROOT,upload_id);
    bool ok = rename(staging,dest)==0;
    if (ok) staging[0]=upload_id[0]=upload_token[0]=0;
    if (ok) {
        upload_holds_player = false;
        player_storage_end();
        ESP_LOGI("upload", "Committed material; HTTP stack remaining: %u bytes",
                 (unsigned)uxTaskGetStackHighWaterMark(NULL));
    }
    return ok ? httpd_resp_sendstr(req,"OK") : fail(req,"500 Internal Server Error","Cannot commit material.");
}
static esp_err_t abort_upload(httpd_req_t *req)
{
    if (!session(req)) return fail(req,"409 Conflict","Upload session expired.");
    storage_api_cleanup(); return httpd_resp_sendstr(req,"OK");
}
static esp_err_t delete_material(httpd_req_t *req)
{
    if (upload_holds_player) return fail(req,"409 Conflict","Finish or cancel the active upload before deleting a material.");
    char id[65], path[PATH_SIZE];
    if (!param(req,"id",id,sizeof(id)) || !storage_api_valid_id(id)) return fail(req,"400 Bad Request","Invalid material ID.");
    if (!player_storage_begin()) return fail(req,"503 Service Unavailable","Player is busy.");
    snprintf(path,sizeof(path),"%s/%s",CARD_ROOT,id);
    remove_material(path);
    struct stat info; bool gone = stat(path,&info)!=0;
    player_storage_end();
    return gone ? httpd_resp_sendstr(req,"OK") : fail(req,"500 Internal Server Error","Cannot remove material folder.");
}
esp_err_t storage_api_register(httpd_handle_t server)
{
    const httpd_uri_t routes[] = {
        {.uri="/api/materials",.method=HTTP_GET,.handler=materials},
        {.uri="/api/upload/start",.method=HTTP_POST,.handler=start},
        {.uri="/api/upload/chunk",.method=HTTP_POST,.handler=chunk},
        {.uri="/api/upload/commit",.method=HTTP_POST,.handler=commit},
        {.uri="/api/upload/abort",.method=HTTP_POST,.handler=abort_upload},
        {.uri="/api/material/delete",.method=HTTP_POST,.handler=delete_material}
    };
    for (unsigned i=0;i<sizeof(routes)/sizeof(routes[0]);++i) {
        esp_err_t err=httpd_register_uri_handler(server,&routes[i]);
        if (err!=ESP_OK) return err;
    }
    return ESP_OK;
}
