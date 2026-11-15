#pragma once
/*
  Telegram map support for the Blind Maze Project.
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

// Telegram setup
const bool TELEGRAM_ENABLED = true;
const char *WIFI_SSID = "TestSan";
const char *WIFI_PASS = "12345678";
const char *BOT_TOKEN = "Bot:Token";
const long long TG_CHAT_ID = 6386715298; // Bot Provided
const bool MAP_SHOW_FULL = false;        // Cheat Mode - Reveals Full Map
const uint32_t TG_POLL_MS = 3000;

volatile bool tgWinPending = false;
long tgOffset = 0;

// Live map
bool tgLive = false;
long tgLiveMsgId = 0;
String tgLastMap;
uint32_t tgLastEdit = 0;
int tgEditFails = 0;

void telegramNotifyWin() { tgWinPending = true; }

String jsonEscape(const String &in)
{
  String out;
  out.reserve(in.length() + 32);
  for (unsigned i = 0; i < in.length(); i++)
  {
    char c = in[i];
    if (c == '\n')
      out += "\\n";
    else if (c == '"')
      out += "\\\"";
    else if (c == '\\')
      out += "\\\\";
    else
      out += c;
  }
  return out;
}

const char *facingName()
{
  const char *names[4] = {"North", "East", "South", "West"};
  return names[facing];
}

// Drawing map
String buildMapText()
{
  String s;
  s.reserve(1000);

  s += "<b>Blind maze map</b>\n";
  s += "You: (" + String(px + 1) + "," + String(py + 1) + ") facing " + facingName() + "\n";
  s += "<pre>";

  for (int y = 0; y <= H; y++)
  {
    // horizontal wall line above row y
    for (int x = 0; x < W; x++)
    {
      bool wall, known;
      if (y == 0)
      {
        wall = cells[idx(x, 0)] & WALLBIT[N];
        known = visited[idx(x, 0)];
      }
      else if (y == H)
      {
        wall = cells[idx(x, H - 1)] & WALLBIT[S];
        known = visited[idx(x, H - 1)];
      }
      else
      {
        wall = cells[idx(x, y)] & WALLBIT[N];
        known = visited[idx(x, y)] || visited[idx(x, y - 1)];
      }
      s += '+';
      s += (wall && (known || MAP_SHOW_FULL)) ? "---" : "   ";
    }
    s += "+\n";
    if (y == H)
      break;

    // rooms of row y with vertical walls
    for (int x = 0; x <= W; x++)
    {
      bool wall, known;
      if (x == 0)
      {
        wall = cells[idx(0, y)] & WALLBIT[WEST];
        known = visited[idx(0, y)];
      }
      else if (x == W)
      {
        wall = cells[idx(W - 1, y)] & WALLBIT[E];
        known = visited[idx(W - 1, y)];
      }
      else
      {
        wall = cells[idx(x, y)] & WALLBIT[WEST];
        known = visited[idx(x, y)] || visited[idx(x - 1, y)];
      }
      s += (wall && (known || MAP_SHOW_FULL)) ? '|' : ' ';
      if (x == W)
        break;

      char c = ' ';
      int i = idx(x, y);
      if (x == px && y == py)
        c = '@';
      else if (x == goalX && y == goalY)
        c = 'G';
      else if (visited[i])
        c = '.';
      s += ' ';
      s += c;
      s += ' ';
    }
    s += '\n';
  }

  s += "</pre>";
  s += "@ you   G goal   . visited";
  return s;
}

// Telegram API
bool tgSend(long long chatId, const String &text, long *msgIdOut = nullptr)
{
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  String url = String("https://api.telegram.org/bot") + BOT_TOKEN + "/sendMessage";
  if (!http.begin(client, url))
    return false;
  http.addHeader("Content-Type", "application/json");

  char idbuf[24];
  snprintf(idbuf, sizeof(idbuf), "%lld", chatId);
  String body = String("{\"chat_id\":") + idbuf + ",\"parse_mode\":\"HTML\",\"text\":\"" + jsonEscape(text) + "\"}";

  int code = http.POST(body);
  if (code == 200 && msgIdOut)
  {
    String resp = http.getString();
    int m = resp.indexOf("\"message_id\":");
    if (m >= 0)
      *msgIdOut = atol(resp.c_str() + m + 13);
  }
  http.end();
  if (code != 200)
    Serial.printf("Telegram send failed: %d\n", code);
  return code == 200;
}

bool tgEdit(long long chatId, long msgId, const String &text)
{
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  String url = String("https://api.telegram.org/bot") + BOT_TOKEN + "/editMessageText";
  if (!http.begin(client, url))
    return false;
  http.addHeader("Content-Type", "application/json");

  char idbuf[24];
  snprintf(idbuf, sizeof(idbuf), "%lld", chatId);
  String body = String("{\"chat_id\":") + idbuf + ",\"message_id\":" + String(msgId) +
                ",\"parse_mode\":\"HTML\",\"text\":\"" + jsonEscape(text) + "\"}";

  int code = http.POST(body);
  http.end();
  if (code != 200)
    Serial.printf("Telegram edit failed: %d\n", code);
  return code == 200;
}

// Live map update
void tgLiveUpdate()
{
  if (!tgLive || TG_CHAT_ID == 0)
    return;
  // Limit updates
  if (millis() - tgLastEdit < 1500)
    return;
  String m = buildMapText();
  if (m == tgLastMap)
    return;
  tgLastEdit = millis();
  if (tgEdit(TG_CHAT_ID, tgLiveMsgId, m))
  {
    tgLastMap = m;
    tgEditFails = 0;
  }
  else if (++tgEditFails >= 5)
  {
    tgLive = false;
  } // message deleted or other problems
}

void tgHandleUpdate(const String &seg)
{
  int c = seg.indexOf("\"chat\":{\"id\":");
  if (c < 0)
    return;
  long long chatId = atoll(seg.c_str() + c + 13);

  // text
  int t = seg.indexOf("\"text\":\"");
  if (t < 0)
    return;
  t += 8;
  int e = seg.indexOf('"', t);
  if (e < 0)
    return;
  String text = seg.substring(t, e);

  if (TG_CHAT_ID == 0)
  {
    char buf[96];
    snprintf(buf, sizeof(buf), "Your chat ID is %lld. Put it in TG_CHAT_ID in telegram_map.h and re-upload.", chatId);
    tgSend(chatId, String(buf));
    Serial.println(buf);
    return;
  }
  if (chatId != TG_CHAT_ID)
    return;

  if (text.startsWith("/map"))
  {
    tgSend(chatId, buildMapText());
  }
  else if (text.startsWith("/live"))
  {
    String m = buildMapText();
    long id = 0;
    if (tgSend(chatId, m, &id) && id)
    {
      tgLive = true;
      tgLiveMsgId = id;
      tgLastMap = m;
      tgLastEdit = millis();
      tgEditFails = 0;
    }
  }
  else if (text.startsWith("/stop"))
  {
    tgLive = false;
    tgSend(chatId, "Live map stopped.");
  }
  else if (text.startsWith("/start") || text.startsWith("/help"))
  {
    tgSend(chatId, "Blind maze bot.\n/map - map of explored rooms\n/live - map that updates by itself\n/stop - stop live map");
  }
}

void tgPoll()
{
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  char url[200];
  snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/getUpdates?offset=%ld&timeout=0", BOT_TOKEN, tgOffset);
  if (!http.begin(client, url))
    return;

  int code = http.GET();
  if (code != 200)
  {
    http.end();
    return;
  }
  String payload = http.getString();
  http.end();

  int pos = 0;
  while (true)
  {
    int u = payload.indexOf("\"update_id\":", pos);
    if (u < 0)
      break;
    long id = atol(payload.c_str() + u + 12);
    int next = payload.indexOf("\"update_id\":", u + 12);
    String seg = (next < 0) ? payload.substring(u) : payload.substring(u, next);
    tgOffset = id + 1;
    tgHandleUpdate(seg);
    if (next < 0)
      break;
    pos = next;
  }
}

// Background task
void telegramTask(void *param)
{
  for (;;)
  {
    if (WiFi.status() != WL_CONNECTED)
    {
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASS);
      uint32_t t0 = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000)
        vTaskDelay(pdMS_TO_TICKS(250));
      if (WiFi.status() != WL_CONNECTED)
      {
        vTaskDelay(pdMS_TO_TICKS(10000));
        continue;
      }
      Serial.print("WiFi connected: ");
      Serial.println(WiFi.localIP());
    }

    if (tgWinPending && TG_CHAT_ID != 0)
    {
      tgWinPending = false;
      tgSend(TG_CHAT_ID, "Maze solved! A new maze has been generated.");
    }

    static uint32_t lastPoll = 0;
    if (millis() - lastPoll >= TG_POLL_MS)
    {
      lastPoll = millis();
      tgPoll();
    }
    tgLiveUpdate();
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

void startTelegram()
{
  if (!TELEGRAM_ENABLED)
    return;
  WiFi.mode(WIFI_STA);
  xTaskCreatePinnedToCore(telegramTask, "telegram", 16384, NULL, 1, NULL, 0);
}
