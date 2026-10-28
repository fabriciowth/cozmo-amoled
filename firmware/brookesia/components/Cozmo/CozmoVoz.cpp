#include "CozmoVoz.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>

#include "CozmoFace.hpp"
#include "XiaozhiAudioProcessor.hpp"
#include "bsp_board_extra.h"
#include "cJSON.h"
#include "esp_ae_rate_cvt.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "esp_websocket_client.h"
#include "esp_wifi.h"
#include "model_path.h"
#include "secrets.h"  // copie secrets.example.h para secrets.h e preencha

namespace esp_brookesia::apps {

namespace {

constexpr char TAG[] = "CozmoVoz";
constexpr uint32_t TAXA_PLACA = 24000, TAXA_AFE = 16000;
constexpr size_t QUADROS = TAXA_PLACA / 100;  // lê o microfone de 10 em 10 ms
constexpr uint32_t SONO_MS = 30000;           // tanto tempo sem ninguém falar e ele dorme
constexpr uint32_t CONECTA_MS = 25000;        // o 1º TLS (RSA 4096 na PSRAM) leva até ~11 s
constexpr uint32_t FALANDO_MS = 300;          // depois do último som, ainda conta como falando
constexpr int GANHO_VOZ = 30;                 // x10: a voz da OpenAI vem baixa. Distorceu? Baixe.
// MultiNet inglês: o g2p transforma o texto em fonemas. Várias grafias pegam sotaques diferentes.
const char *WAKE_WORDS[] = {"hey cozmo", "hey cosmo", "hi cosmo", "okay cosmo", "acorda cosmo", "a corda cosmo"};
// Eventos do corpo dele que viram fala: o agente recebe como mensagem e reage.
const char TONTO[] = R"J({"type":"InjectUserMessage","content":"[evento, nao e fala do usuario: acabaram de )J"
                     R"J(chacoalhar sua telinha e voce ficou tonto, vendo estrelinhas. Reaja com UMA frase curta )J"
                     R"J(e engracada, sem chamar set_expression.]"})J";
const char CARINHO[] = R"J({"type":"InjectUserMessage","content":"[evento, nao e fala do usuario: acabaram de )J"
                       R"J(fazer carinho na sua telinha. Voce adora. Reaja com UMA frase curta e fofa, sem chamar )J"
                       R"J(set_expression.]"})J";
const char *HUMORES[] = {"neutro", "feliz", "triste", "bravo", "sono", "curioso", "surpreso", "pensando"};

// Mesmo agente do cozmo_ype: Flux ouve, gpt-4o-mini pensa e a voz "echo" da OpenAI fala (o Aura não fala português).
const char SETTINGS[] = R"({"type":"Settings",
 "audio":{"input":{"encoding":"linear16","sample_rate":16000},
          "output":{"encoding":"linear16","sample_rate":24000,"container":"none"}},
 "agent":{
  "listen":{"provider":{"type":"deepgram","version":"v2","model":"flux-general-multi","language_hints":["pt","en"]}},
  "think":{"provider":{"type":"open_ai","model":"gpt-4o-mini","temperature":0.6},
   "prompt":"Voce e o Cozmo, um robozinho curioso, simpatico e bem-humorado que mora numa telinha redonda. Fale SEMPRE portugues do Brasil. Hey Cozmo e Hey Cosmo sao so o seu nome, nao uma frase em ingles: continue em portugues. So troque de lingua se pedirem explicitamente. Seja natural e curto: uma ou duas frases, porque sua resposta vira voz. Nada de listas, markdown ou emoji. Use set_expression para mostrar o que sente: feliz quando elogiado ou com boa noticia, triste com noticia ruim, surpreso com algo inesperado, bravo de brincadeira, curioso quando quer saber mais. Na maior parte do tempo fique neutro. Se perguntarem, diga que balancar a telinha te deixa tonto. Voce mora em )" CIDADE R"(. DADOS REAIS: para hora, data e clima use SEMPRE get_time e get_weather e nunca invente numeros; fale o resultado de um jeito facil de ouvir, com numeros arredondados. VOLUME: se pedirem para falar mais alto, mais baixo ou num volume, use set_volume.",
   "functions":[{"name":"set_expression","description":"Muda a expressao do rosto do Cozmo",
    "parameters":{"type":"object","properties":{"mood":{"type":"string",
     "enum":["neutro","feliz","triste","bravo","sono","curioso","surpreso","pensando"]}},"required":["mood"]}},
    {"name":"get_time","description":"Hora e data atuais, horario de Brasilia","parameters":{"type":"object","properties":{}}},
    {"name":"get_weather","description":"Clima agora e previsao de hoje e amanha na cidade do Cozmo",
     "parameters":{"type":"object","properties":{}}},
    {"name":"set_volume","description":"Muda o volume do alto-falante do Cozmo",
     "parameters":{"type":"object","properties":{
      "acao":{"type":"string","enum":["aumentar","diminuir","definir"]},
      "nivel":{"type":"integer","description":"0 a 100, so para definir"}},"required":["acao"]}}]},
  "speak":{"provider":{"type":"open_ai","model":"tts-1","voice":"echo"},
   "endpoint":{"url":"https://api.openai.com/v1/audio/speech","headers":{"authorization":"Bearer )" OPENAI_KEY R"("}}},
  "greeting":"Oi! To acordado."}})";

uint32_t agora()
{
    return xTaskGetTickCount() * portTICK_PERIOD_MS;
}

std::string httpGet(const char *url)
{
    std::string corpo;
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = 6000;
    cfg.user_data = &corpo;
    cfg.event_handler = [](esp_http_client_event_t *e) {
        if (e->event_id == HTTP_EVENT_ON_DATA) static_cast<std::string *>(e->user_data)->append((char *)e->data, e->data_len);
        return ESP_OK;
    };
    auto h = esp_http_client_init(&cfg);
    bool ok = esp_http_client_perform(h) == ESP_OK && esp_http_client_get_status_code(h) == 200;
    esp_http_client_cleanup(h);
    return ok ? corpo : "";
}

std::string hora()
{
    time_t t = time(nullptr);
    struct tm tm;
    localtime_r(&t, &tm);
    if (tm.tm_year + 1900 < 2025) return "relogio ainda nao sincronizado com a internet";
    static const char *DIAS[] = {"domingo", "segunda", "terca", "quarta", "quinta", "sexta", "sabado"};
    char s[64];
    snprintf(s, sizeof(s), "%02d:%02d, %s, %02d/%02d/%d", tm.tm_hour, tm.tm_min, DIAS[tm.tm_wday], tm.tm_mday,
             tm.tm_mon + 1, tm.tm_year + 1900);
    return s;
}

// Códigos WMO do Open-Meteo
const char *tempo(int c)
{
    return c == 0 ? "ceu limpo" : c <= 3 ? "parcialmente nublado" : c <= 48 ? "neblina" : c <= 57 ? "garoa"
         : c <= 67 ? "chuva" : c <= 77 ? "neve" : c <= 82 ? "pancadas de chuva" : "trovoadas";
}

std::string clima()
{
    std::string r = httpGet("https://api.open-meteo.com/v1/forecast?latitude=" LAT "&longitude=" LON
                            "&current=temperature_2m,apparent_temperature,relative_humidity_2m,weather_code,wind_speed_10m"
                            "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max"
                            "&timezone=auto&forecast_days=2");
    cJSON *j = cJSON_Parse(r.c_str());
    if (!j) return "nao consegui consultar o clima agora";
    auto num = [](cJSON *o, const char *k, int i = -1) {
        cJSON *v = cJSON_GetObjectItem(o, k);
        if (i >= 0) v = cJSON_GetArrayItem(v, i);
        return cJSON_IsNumber(v) ? v->valuedouble : 0.0;
    };
    cJSON *c = cJSON_GetObjectItem(j, "current"), *d = cJSON_GetObjectItem(j, "daily");
    char s[400];
    snprintf(s, sizeof(s),
             "Agora em " CIDADE ": %.0f graus (sensacao %.0f), umidade %.0f%%, vento %.0f km/h, %s. "
             "Hoje: minima %.0f, maxima %.0f, chance de chuva %.0f%%, %s. "
             "Amanha: minima %.0f, maxima %.0f, chance de chuva %.0f%%, %s.",
             num(c, "temperature_2m"), num(c, "apparent_temperature"), num(c, "relative_humidity_2m"),
             num(c, "wind_speed_10m"), tempo((int)num(c, "weather_code")),
             num(d, "temperature_2m_min", 0), num(d, "temperature_2m_max", 0), num(d, "precipitation_probability_max", 0),
             tempo((int)num(d, "weather_code", 0)),
             num(d, "temperature_2m_min", 1), num(d, "temperature_2m_max", 1), num(d, "precipitation_probability_max", 1),
             tempo((int)num(d, "weather_code", 1)));
    cJSON_Delete(j);
    return s;
}

std::string volume(cJSON *args)
{
    const char *acao = cJSON_GetStringValue(cJSON_GetObjectItem(args, "acao"));
    cJSON *nivel = cJSON_GetObjectItem(args, "nivel");
    int v = bsp_extra_codec_volume_get();
    if (acao && !strcmp(acao, "aumentar")) v += 20;
    else if (acao && !strcmp(acao, "diminuir")) v -= 20;
    else if (cJSON_IsNumber(nivel)) v = nivel->valueint;
    v = v < 10 ? 10 : v > 100 ? 100 : v;  // nunca mudo: senão ele some de vez
    bsp_extra_codec_volume_set(v, nullptr);
    return "volume agora em " + std::to_string(v) + " de 100" + (v == 100 ? " (o maximo)" : "");
}

} // namespace

bool CozmoVoz::start(CozmoFace *rosto)
{
    if (_rodando) return true;
    _rosto = rosto;
    if (bsp_extra_audio_session_acquire(BSP_EXTRA_AUDIO_OWNER_COZMO) != ESP_OK) {
        ESP_LOGW(TAG, "audio ocupado por %s", bsp_extra_audio_owner_name(bsp_extra_audio_session_get_owner()));
        _status = "Áudio ocupado: feche o AIChats";
        return false;
    }
    if (bsp_extra_codec_set_voice_fs(TAXA_PLACA, 16, CODEC_VOICE_INPUT_CHANNELS, BSP_EXTRA_ES7210_TDM_ALL_SLOTS_MASK,
                                     BSP_EXTRA_ES7210_PHYSICAL_CONNECTED_MIC_MASK) != ESP_OK) {
        bsp_extra_audio_session_release(BSP_EXTRA_AUDIO_OWNER_COZMO);
        _status = "Falha no áudio";
        return false;
    }
    // O mesmo front-end do AIChats: dois microfones + referência do alto-falante (cancela o eco), 16 kHz mono.
    _afe = new XiaozhiAudioProcessor();
    _pedidos = xQueueCreate(4, sizeof(char *));
    _ouvido = xStreamBufferCreateWithCaps(TAXA_AFE * 2, 1, MALLOC_CAP_SPIRAM);           // 1 s
    _boca = xStreamBufferCreateWithCaps(TAXA_PLACA * 2 * 10, 1, MALLOC_CAP_SPIRAM);      // 10 s de voz
    if (!_afe->initialize()) {
        _status = "Falha no microfone";
        stop();
        return false;
    }
    _afe->setPcmFrameCallback([this](const int16_t *pcm, size_t n) { xStreamBufferSend(_ouvido, pcm, n * 2, 0); });
    _afe->enableWakeWord(false);
    _afe->enableVoiceProcessing(true);

    srmodel_list_t *modelos = esp_srmodel_init("model");
    char *nome = modelos ? esp_srmodel_filter(modelos, ESP_MN_PREFIX, ESP_MN_ENGLISH) : nullptr;
    if (nome) {
        auto *mn = esp_mn_handle_from_name(nome);
        auto *dados = mn->create(nome, 3000);
        esp_mn_commands_alloc(mn, dados);
        for (size_t i = 0; i < sizeof(WAKE_WORDS) / sizeof(WAKE_WORDS[0]); i++) esp_mn_commands_add(1, WAKE_WORDS[i]);
        esp_mn_commands_update();
        mn->print_active_speech_commands(dados);
        _mn = (void *)mn;
        _mn_dados = dados;
    } else {
        ESP_LOGW(TAG, "sem MultiNet ingles na particao model: so acorda com toque");
    }

    _rodando = true;
    _estado = DORMINDO;
    _calar = false;
    _status = _mn ? "Zzz... diga \"Hey Cozmo\"" : "Zzz... toque para acordar";
    _rosto->setHumor(CozmoFace::SONO);
    _tonturas_vistas = _rosto->tonturas();
    _tarefas = 3;
    // o eco é cancelado dentro do feed(): pilha na RAM interna e núcleo 0, como o xiaozhi_input do AIChats
    xTaskCreatePinnedToCore(tarefaCaptura, "cozmo_mic", 4096, this, 8, nullptr, 0);
    xTaskCreatePinnedToCoreWithCaps(tarefaCerebro, "cozmo_cerebro", 12288, this, 4, nullptr, 1, MALLOC_CAP_SPIRAM);
    xTaskCreatePinnedToCoreWithCaps(tarefaFala, "cozmo_fala", 4096, this, 6, nullptr, 1, MALLOC_CAP_SPIRAM);
    return true;
}

void CozmoVoz::stop()
{
    _rodando = false;
    for (int i = 0; i < 300 && _tarefas > 0; i++) vTaskDelay(pdMS_TO_TICKS(10));
    desconecta();
    if (_afe) {
        _afe->shutdown();
        delete _afe;
        _afe = nullptr;
    }
    if (_mn) {
        esp_mn_commands_free();
        static_cast<esp_mn_iface_t *>(_mn)->destroy(static_cast<model_iface_data_t *>(_mn_dados));
        _mn = _mn_dados = nullptr;
    }
    if (bsp_extra_audio_session_get_owner() == BSP_EXTRA_AUDIO_OWNER_COZMO) {
        bsp_extra_codec_dev_stop();
        bsp_extra_audio_session_release(BSP_EXTRA_AUDIO_OWNER_COZMO);
    }
    if (_pedidos) {
        char *p;
        while (xQueueReceive(_pedidos, &p, 0)) cJSON_free(p);
        vQueueDelete(_pedidos);
        _pedidos = nullptr;
    }
    for (auto *s : {&_ouvido, &_boca}) {
        if (*s) vStreamBufferDeleteWithCaps(*s);
        *s = nullptr;
    }
}

void CozmoVoz::toque()
{
    if (!_rodando) return;
    if (_estado == DORMINDO) {
        conecta();
    } else if (agora() - _ultimo_som < FALANDO_MS) {
        _calar = true;  // descarta o resto da fala; a próxima resposta libera de novo
    }
}

void CozmoVoz::tarefaCaptura(void *arg)
{
    static_cast<CozmoVoz *>(arg)->captura();
    static_cast<CozmoVoz *>(arg)->_tarefas--;
    vTaskDelete(nullptr);
}

void CozmoVoz::tarefaCerebro(void *arg)
{
    static_cast<CozmoVoz *>(arg)->cerebro();
    static_cast<CozmoVoz *>(arg)->_tarefas--;
    vTaskDeleteWithCaps(nullptr);
}

void CozmoVoz::tarefaFala(void *arg)
{
    static_cast<CozmoVoz *>(arg)->fala();
    static_cast<CozmoVoz *>(arg)->_tarefas--;
    vTaskDeleteWithCaps(nullptr);
}

// Microfone: TDM de 4 canais a 24 kHz -> MIC1, MIC2, eco a 16 kHz, que é o que o AFE espera.
void CozmoVoz::captura()
{
    esp_ae_rate_cvt_cfg_t cfg = {};
    cfg.src_rate = TAXA_PLACA;
    cfg.dest_rate = TAXA_AFE;
    cfg.channel = 3;
    cfg.bits_per_sample = 16;
    cfg.complexity = 2;
    cfg.perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED;
    esp_ae_rate_cvt_handle_t conversor = nullptr;
    uint32_t max_saida = 0;
    esp_ae_rate_cvt_open(&cfg, &conversor);
    if (conversor) esp_ae_rate_cvt_get_max_out_sample_num(conversor, QUADROS, &max_saida);
    std::vector<int16_t> tdm(QUADROS * CODEC_VOICE_INPUT_CHANNELS), tres(QUADROS * 3), saida(max_saida * 3);
    while (_rodando && conversor) {
        size_t lidos = 0;
        if (bsp_extra_i2s_read(tdm.data(), tdm.size() * 2, &lidos, 100) != ESP_OK || !lidos) {
            vTaskDelay(1);
            continue;
        }
        uint32_t n = lidos / (CODEC_VOICE_INPUT_CHANNELS * 2);
        for (uint32_t f = 0; f < n; f++) {
            tres[f * 3] = tdm[f * CODEC_VOICE_INPUT_CHANNELS];          // MIC1
            tres[f * 3 + 1] = tdm[f * CODEC_VOICE_INPUT_CHANNELS + 2];  // MIC2
            tres[f * 3 + 2] = tdm[f * CODEC_VOICE_INPUT_CHANNELS + 1];  // eco do alto-falante
        }
        uint32_t m = max_saida;
        if (esp_ae_rate_cvt_process(conversor, (esp_ae_sample_t)tres.data(), n, (esp_ae_sample_t)saida.data(), &m) ==
                ESP_AE_ERR_OK && m) {
            _afe->feed(saida.data(), m);
        }
    }
    if (conversor) esp_ae_rate_cvt_close(conversor);
}

// Dormindo: procura a wake word. Acordado: manda o microfone para o Deepgram e cuida do sono.
void CozmoVoz::cerebro()
{
    int16_t quadro[XiaozhiAudioProcessor::PCM_FRAME_SAMPLES];
    while (_rodando) {
        size_t bytes = xStreamBufferReceive(_ouvido, quadro, sizeof(quadro), pdMS_TO_TICKS(100));
        uint32_t t = agora();
        bool falando = t - _ultimo_som < FALANDO_MS;
        _rosto->setFalando(falando);
        if (falando && (_rosto->humor() == CozmoFace::PENSANDO || _rosto->humor() == CozmoFace::CURIOSO)) {
            _rosto->setHumor(CozmoFace::NEUTRO);  // começou a responder sem pedir outra expressão
        }
        char *pedido;
        while (xQueueReceive(_pedidos, &pedido, 0)) {  // HTTP do clima aqui, fora da tarefa do websocket
            atende(pedido);
            cJSON_free(pedido);
        }
        int estado = _estado;
        if (_rosto->tonturas() != _tonturas_vistas) {  // chacoalharam
            _tonturas_vistas = _rosto->tonturas();
            _evento = TONTO;
        }
        if (const char *ev = _evento.exchange(nullptr)) {  // ele reage falando; se dormia, acorda para reagir
            if (estado == CONVERSANDO) {
                injeta(ev);
            } else {
                _pendente = ev;
                if (estado == DORMINDO) conecta();
            }
            continue;
        }
        if (estado == DORMINDO) {
            if (bytes && ouviuWakeWord(quadro, bytes / 2)) conecta();
            continue;
        }
        bool quieto = !falando && t - _ultima_atividade > (estado == CONECTANDO ? CONECTA_MS : SONO_MS);
        if (estado == DESLIGANDO && !quieto && !_reconexao) {
            // caiu no meio da conversa (WiFi fraco): reconecta uma vez, sem repetir o "oi"
            ESP_LOGW(TAG, "reconectando");
            desconecta();
            _reconexao = true;
            conecta();
            continue;
        }
        if (estado == DESLIGANDO || quieto) {
            desconecta();
            continue;
        }
        if (estado == CONVERSANDO && bytes && esp_websocket_client_is_connected((esp_websocket_client_handle_t)_ws)) {
            // Enquanto ele fala, o microfone não vai: senão ele se ouve pelo alto-falante e responde a si
            // mesmo. Nem silêncio vai: com WiFi fraco, subir áudio enquanto desce a voz trava a conexão.
            static uint32_t ultimo_keepalive = 0;
            if (falando) {
                if (t - ultimo_keepalive > 5000) {
                    esp_websocket_client_send_text((esp_websocket_client_handle_t)_ws, "{\"type\":\"KeepAlive\"}", 20,
                                                   pdMS_TO_TICKS(4000));
                    ultimo_keepalive = t;
                }
                continue;
            }
            static uint32_t enviados = 0, falhas = 0, pico = 0, ultimo_log = 0;
            for (size_t i = 0; i < bytes / 2; i++) pico = std::max<uint32_t>(pico, abs(quadro[i]));
            if (t - ultimo_log > 3000) {
                ESP_LOGI(TAG, "mic -> deepgram: %lu quadros, %lu falhas, pico %lu", (unsigned long)enviados,
                         (unsigned long)falhas, (unsigned long)pico);
                enviados = falhas = pico = 0;
                ultimo_log = t;
            }
            enviados++;
            if (esp_websocket_client_send_bin((esp_websocket_client_handle_t)_ws, (const char *)quadro, bytes,
                                          pdMS_TO_TICKS(4000)) < 0) {  // WiFi fraco: um envio lento derruba a conexão
                falhas++;
            }
        }
    }
}

// Alto-falante: voz mono 24 kHz do Deepgram -> estéreo para o ES8311.
void CozmoVoz::fala()
{
    int16_t mono[480], estereo[960];
    while (_rodando) {
        size_t bytes = xStreamBufferReceive(_boca, mono, sizeof(mono), pdMS_TO_TICKS(50));
        if (!bytes || _calar) continue;
        size_t n = bytes / 2, escritos = 0;
        for (size_t i = 0; i < n; i++) {
            int v = mono[i] * GANHO_VOZ / 10;
            estereo[2 * i] = estereo[2 * i + 1] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        }
        bsp_extra_i2s_write(estereo, n * 4, &escritos, 200);
        _ultimo_som = _ultima_atividade = agora();
    }
}

bool CozmoVoz::ouviuWakeWord(const int16_t *pcm, size_t n)
{
    auto *mn = static_cast<esp_mn_iface_t *>(_mn);
    auto *dados = static_cast<model_iface_data_t *>(_mn_dados);
    if (!mn) return false;
    static std::vector<int16_t> acumulado;
    acumulado.insert(acumulado.end(), pcm, pcm + n);
    size_t pedaco = mn->get_samp_chunksize(dados);
    bool ouviu = false;
    while (acumulado.size() >= pedaco && !ouviu) {
        esp_mn_state_t estado = mn->detect(dados, acumulado.data());
        if (estado == ESP_MN_STATE_DETECTED) {
            esp_mn_results_t *r = mn->get_results(dados);
            ESP_LOGI(TAG, "wake word: %s (%.2f)", r->string, r->prob[0]);
            ouviu = true;
        }
        if (estado == ESP_MN_STATE_DETECTED || estado == ESP_MN_STATE_TIMEOUT) mn->clean(dados);
        acumulado.erase(acumulado.begin(), acumulado.begin() + pedaco);
    }
    if (ouviu) acumulado.clear();
    return ouviu;
}

void CozmoVoz::conecta()
{
    if (_ws) return;
    esp_websocket_client_config_t cfg = {};
    cfg.uri = "wss://agent.deepgram.com/v1/agent/converse";
    cfg.headers = "Authorization: Token " DEEPGRAM_KEY "\r\n";
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.buffer_size = 4096;
    cfg.task_stack = 6144;
    cfg.disable_auto_reconnect = true;
    cfg.network_timeout_ms = 10000;
    auto ws = esp_websocket_client_init(&cfg);
    if (!ws) return;
    esp_websocket_register_events(ws, WEBSOCKET_EVENT_ANY, eventoWs, this);
    _ws = ws;
    _calar = false;
    _ultima_atividade = agora();
    _estado = CONECTANDO;
    _status = "Acordando...";
    esp_wifi_set_ps(WIFI_PS_NONE);  // a economia do WiFi faz o envio contínuo do microfone engasgar
    _rosto->setHumor(CozmoFace::SURPRESO);
    if (esp_websocket_client_start(ws) != ESP_OK) _estado = DESLIGANDO;
}

void CozmoVoz::desconecta()
{
    if (_ws) {
        auto ws = (esp_websocket_client_handle_t)_ws;
        _ws = nullptr;
        if (esp_websocket_client_is_connected(ws)) esp_websocket_client_close(ws, pdMS_TO_TICKS(1000));
        esp_websocket_client_destroy(ws);
    }
    _calar = true;
    _reconexao = false;
    _pendente = nullptr;
    _estado = DORMINDO;
    _status = _mn ? "Zzz... diga \"Hey Cozmo\"" : "Zzz... toque para acordar";
    if (_rosto) _rosto->setHumor(CozmoFace::SONO);
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
}

void CozmoVoz::eventoWs(void *arg, const char *, int32_t id, void *dados)
{
    auto *self = static_cast<CozmoVoz *>(arg);
    auto *d = static_cast<esp_websocket_event_data_t *>(dados);
    static bool binario = false;  // frames de continuação (op 0) herdam o tipo do primeiro
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        if (self->_reconexao || self->_pendente) {  // mesmo agente, sem a saudação
            std::string s = SETTINGS;
            s.erase(s.find(",\n  \"greeting\""), std::string::npos);
            s += "}}";
            esp_websocket_client_send_text(d->client, s.c_str(), s.size(), pdMS_TO_TICKS(2000));
        } else {
            esp_websocket_client_send_text(d->client, SETTINGS, sizeof(SETTINGS) - 1, pdMS_TO_TICKS(2000));
        }
        break;
    case WEBSOCKET_EVENT_DATA:
        if (d->op_code == 0x1 || d->op_code == 0x2) binario = d->op_code == 0x2;
        else if (d->op_code != 0x0) break;  // ping, pong, close
        if (binario) {
            if (!self->_calar && d->data_len > 0) {
                xStreamBufferSend(self->_boca, d->data_ptr, d->data_len, pdMS_TO_TICKS(2000));
            }
        } else {
            if (d->payload_offset == 0) self->_texto.clear();
            self->_texto.append(d->data_ptr, d->data_len);
            if (d->payload_offset + d->data_len >= d->payload_len) self->mensagem(self->_texto);
        }
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
    case WEBSOCKET_EVENT_ERROR:
        if (self->_estado != DORMINDO) {
            ESP_LOGW(TAG, "conexao com o Deepgram caiu (evento %ld)", (long)id);
            self->_estado = DESLIGANDO;
        }
        break;
    }
}

void CozmoVoz::mensagem(const std::string &json)
{
    cJSON *j = cJSON_Parse(json.c_str());
    if (!j) return;
    const char *tipo = cJSON_GetStringValue(cJSON_GetObjectItem(j, "type"));
    tipo = tipo ? tipo : "";
    if (strcmp(tipo, "ConversationText") && strcmp(tipo, "LatencyReport")) ESP_LOGI(TAG, "<- %.300s", json.c_str());
    uint32_t t = agora();
    if (!strcmp(tipo, "SettingsApplied")) {
        _estado = CONVERSANDO;
        _status = "";
        _ultima_atividade = t;
        if (!_reconexao) _rosto->setHumor(CozmoFace::FELIZ);
        _reconexao = false;  // voltou a andar: se cair de novo, tenta reconectar de novo
        if (const char *ev = _pendente.exchange(nullptr)) injeta(ev);
    } else if (!strcmp(tipo, "UserStartedSpeaking")) {
        _calar = true;  // você começou a falar: ele para
        _ultima_atividade = t;
        _rosto->setHumor(CozmoFace::CURIOSO);
    } else if (!strcmp(tipo, "AgentThinking") || !strcmp(tipo, "EndOfTurn")) {
        // o Flux avisa EndOfTurn quando você termina e não manda AgentStartedSpeaking: a voz já pode tocar
        _calar = false;
        _rosto->setHumor(CozmoFace::PENSANDO);
    } else if (!strcmp(tipo, "AgentStartedSpeaking")) {
        _calar = false;
        _ultima_atividade = t;
        if (_rosto->humor() == CozmoFace::PENSANDO || _rosto->humor() == CozmoFace::CURIOSO) {
            _rosto->setHumor(CozmoFace::NEUTRO);
        }
    } else if (!strcmp(tipo, "ConversationText")) {
        _ultima_atividade = t;
        ESP_LOGI(TAG, "%s: %s", cJSON_GetStringValue(cJSON_GetObjectItem(j, "role")),
                 cJSON_GetStringValue(cJSON_GetObjectItem(j, "content")));
    } else if (!strcmp(tipo, "FunctionCallRequest")) {
        cJSON *f;
        cJSON_ArrayForEach(f, cJSON_GetObjectItem(j, "functions")) {
            char *pedido = cJSON_PrintUnformatted(f);
            if (xQueueSend(_pedidos, &pedido, 0) != pdTRUE) cJSON_free(pedido);
        }
    } else if (!strcmp(tipo, "Error") || !strcmp(tipo, "Warning")) {
        ESP_LOGW(TAG, "Deepgram: %s", json.c_str());
        if (!strcmp(tipo, "Error")) _status = "Erro no Deepgram (veja o log)";
    }
    cJSON_Delete(j);
}

} // namespace esp_brookesia::apps

namespace esp_brookesia::apps {

void CozmoVoz::atende(const char *pedido)
{
    cJSON *f = cJSON_Parse(pedido);
    const char *nome = cJSON_GetStringValue(cJSON_GetObjectItem(f, "name"));
    cJSON *args = cJSON_Parse(cJSON_GetStringValue(cJSON_GetObjectItem(f, "arguments")));
    nome = nome ? nome : "";
    std::string resposta = "ok";
    if (!strcmp(nome, "set_expression")) {
        const char *humor = cJSON_GetStringValue(cJSON_GetObjectItem(args, "mood"));
        for (int h = 0; humor && h < CozmoFace::NHUMORES; h++) {
            if (!strcmp(humor, HUMORES[h])) _rosto->setHumor((CozmoFace::Humor)h);
        }
    } else if (!strcmp(nome, "get_time")) {
        resposta = hora();
    } else if (!strcmp(nome, "get_weather")) {
        resposta = clima();
    } else if (!strcmp(nome, "set_volume")) {
        resposta = volume(args);
    }
    ESP_LOGI(TAG, "%s -> %s", nome, resposta.c_str());
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "type", "FunctionCallResponse");
    cJSON_AddStringToObject(r, "id", cJSON_GetStringValue(cJSON_GetObjectItem(f, "id")));
    cJSON_AddStringToObject(r, "name", nome);
    cJSON_AddStringToObject(r, "content", resposta.c_str());
    char *s = cJSON_PrintUnformatted(r);
    if (_ws) esp_websocket_client_send_text((esp_websocket_client_handle_t)_ws, s, strlen(s), pdMS_TO_TICKS(2000));
    cJSON_free(s);
    cJSON_Delete(r);
    cJSON_Delete(args);
    cJSON_Delete(f);
}

void CozmoVoz::injeta(const char *msg)
{
    if (_ws) esp_websocket_client_send_text((esp_websocket_client_handle_t)_ws, msg, strlen(msg), pdMS_TO_TICKS(2000));
}

void CozmoVoz::carinho()
{
    _rosto->setHumor(CozmoFace::FELIZ);
    if (_rodando) _evento = CARINHO;
}

} // namespace esp_brookesia::apps
