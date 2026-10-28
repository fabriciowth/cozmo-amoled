// Voz do app Cozmo: dorme ouvindo só a wake word ("hey cozmo", "acorda cozmo", em MultiNet na placa);
// acordado, conversa pelo Deepgram Voice Agent (Flux ouve, gpt-4o-mini pensa, OpenAI tts-1 fala),
// igual ao celular do cozmo_ype. Depois de um tempo em silêncio, desliga a conexão e volta a dormir.
#pragma once

#include <atomic>
#include <string>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

namespace esp_brookesia::apps {

class CozmoFace;
class XiaozhiAudioProcessor;

class CozmoVoz {
public:
    bool start(CozmoFace *rosto);  // pega o áudio da placa e começa dormindo
    void stop();
    void toque();                  // dormindo: acorda. Falando: cala a boca.
    void carinho();                // dedo segurado na tela: fica feliz e reage
    const char *status() const { return _status.load(); }

private:
    enum Estado { DORMINDO, CONECTANDO, CONVERSANDO, DESLIGANDO };

    static void tarefaCaptura(void *arg);
    static void tarefaCerebro(void *arg);
    static void tarefaFala(void *arg);
    static void eventoWs(void *arg, const char *base, int32_t id, void *dados);
    void captura();
    void cerebro();
    void fala();
    void mensagem(const std::string &json);
    void conecta();
    void desconecta();
    bool ouviuWakeWord(const int16_t *pcm, size_t n);
    void atende(const char *pedido);  // função pedida pelo agente (hora, clima, volume, expressão)
    void injeta(const char *msg);

    CozmoFace *_rosto = nullptr;
    XiaozhiAudioProcessor *_afe = nullptr;
    void *_ws = nullptr;
    void *_mn = nullptr, *_mn_dados = nullptr;  // MultiNet (esp_mn_iface_t / model_iface_data_t)
    StreamBufferHandle_t _ouvido = nullptr, _boca = nullptr;  // mic 16 kHz / voz 24 kHz
    std::atomic<bool> _rodando{false}, _calar{false}, _reconexao{false};
    std::atomic<const char *> _evento{nullptr}, _pendente{nullptr};  // reação a mandar / a mandar ao conectar
    QueueHandle_t _pedidos = nullptr;
    uint32_t _tonturas_vistas = 0;
    std::atomic<int> _estado{DORMINDO}, _tarefas{0};
    std::atomic<uint32_t> _ultima_atividade{0}, _ultimo_som{0};
    std::atomic<const char *> _status{""};
    std::string _texto;  // mensagem de texto do Deepgram chegando em pedaços
};

} // namespace esp_brookesia::apps
