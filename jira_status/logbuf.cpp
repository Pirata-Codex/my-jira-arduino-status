#include "config.h"
#include <esp_attr.h>
#include <stdarg.h>

// Every LOG line also goes into a ring buffer that the page /log shows. The ring lives in RTC memory,
// which a restart (crash, watchdog, most brownouts) leaves alone, so the lines that led up to a reset
// are still there after it. A real power-off clears it; the magic word tells garbage from a log.

static const uint32_t MAGIC = 0x4C4F4721;
static const uint32_t SIZE = 3072;
RTC_NOINIT_ATTR static struct {
  uint32_t magic, head, len;  // head: where the next byte goes; len: bytes in use (<= SIZE)
  char buf[SIZE];
} ring;
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
static bool checked = false;

static void append(const char* s, size_t n) {
  portENTER_CRITICAL(&mux);
  if (!checked) {  // the first LOG runs in setup(), before any other task logs
    checked = true;
    if (ring.magic != MAGIC || ring.head >= SIZE || ring.len > SIZE) {
      ring.magic = MAGIC;
      ring.head = ring.len = 0;
    }
  }
  for (size_t i = 0; i < n; i++) {
    ring.buf[ring.head] = s[i];
    ring.head = (ring.head + 1) % SIZE;
  }
  ring.len = min<uint32_t>(SIZE, ring.len + n);
  portEXIT_CRITICAL(&mux);
}

void logbuf_printf(const char* fmt, ...) {
  char line[320];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(line, sizeof line - 1, fmt, ap);
  va_end(ap);
  n = constrain(n, 0, (int)sizeof line - 2);
  if (!n || line[n - 1] != '\n') line[n++] = '\n';  // LOG ends its lines with one; a cut line has none
  line[n] = 0;
  Serial.print(line);
  append(line, n);
}

String logbuf_text() {
  String out;
  char* copy = (char*)heap_caps_malloc(SIZE, MALLOC_CAP_SPIRAM);  // keep the internal RAM for TLS
  if (!copy) copy = (char*)malloc(SIZE);
  if (!copy) return "out of memory";
  portENTER_CRITICAL(&mux);
  uint32_t len = ring.len, start = (ring.head + SIZE - len) % SIZE;
  for (uint32_t i = 0; i < len; i++) copy[i] = ring.buf[(start + i) % SIZE];
  portEXIT_CRITICAL(&mux);
  // The oldest line was probably cut by the wrap: start at the first whole one.
  uint32_t from = 0;
  if (len == SIZE)
    while (from < len && copy[from++] != '\n') {}
  out.concat(copy + from, len - from);
  free(copy);
  return out;
}
