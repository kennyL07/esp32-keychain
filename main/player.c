#include "player.h"
#include "board.h"
#include "usb_disk.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define maxCards 128
#define maxPath 537
static char (*cards)[maxPath];

esp_err_t playerInit(void)
{
    cards = heap_caps_calloc(maxCards, sizeof(*cards), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return cards ? ESP_OK : ESP_ERR_NO_MEM;
}

static int compareCards(const void *a, const void *b) { return strcmp(a, b); }

static unsigned scanCards(void)
{
    unsigned count = 0;
    DIR *dir = opendir(cardRoot);
    if (!dir) return count;
    struct dirent *entry;
    while (!usbDiskRequested() && count < maxCards && (entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        char path[maxPath + 16];
        int n = snprintf(path, sizeof(path), "%s/%s/loop.epv", cardRoot, entry->d_name);
        if (n < 0 || n >= maxPath) continue;
        struct stat info;
        if (stat(path, &info) != 0 || !S_ISREG(info.st_mode)) continue;
        n -= sizeof("/loop.epv") - 1;
        memcpy(cards[count], path, n);
        cards[count][n] = 0;
        ++count;
    }
    closedir(dir);
    qsort(cards, count, sizeof(*cards), compareCards);
    return count;
}

void playerRun(void)
{
    if (!usbDiskLocal()) {
        boardStatus("SD CARD ERROR", usbDiskError(), "");
        return;
    }
    unsigned count = scanCards();
    ESP_LOGI("player", "catalog has %u entries", count);
    if (count) ESP_LOGI("player", "first clip: %s", cards[0]);
    if (!count) boardStatus("NO MEDIA", "", "");
}
