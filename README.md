# Cozmo AMOLED

O rosto animado do [Cozmo ESP32](https://github.com/fabriciowth/cozmo-esp32) numa telinha redonda AMOLED,
agora **conversando por voz em português**, com wake word, hora, clima, controle de volume e reações
quando você chacoalha ou faz carinho nele.

Roda na **Waveshare ESP32-S3-Touch-AMOLED-1.75** em cima do firmware de fábrica da Waveshare
(ESP-Brookesia). Todos os apps originais continuam lá, e o Cozmo entra como mais um app na tela inicial.

## O que ele faz

- **"Hey Cozmo"** ou **"Acorda Cozmo"** acorda o Cozmo. A wake word é reconhecida na própria placa, sem internet.
- **Conversa por voz** pelo Deepgram Voice Agent:
  - o Flux ouve;
  - o `gpt-4o-mini` pensa;
  - a voz `echo` da OpenAI fala.
- **Rosto com olhos e boca:**
  - a boca mexe enquanto ele fala;
  - fica curioso quando você fala e pensativo enquanto prepara a resposta;
  - muda de humor sozinho: feliz, triste, bravo, surpreso…
- **Hora e data** com o relógio sincronizado pela internet, no horário de Brasília.
- **Clima de agora e previsão de hoje e de amanhã** pelo Open-Meteo, que não pede chave.
- **Volume por voz:** "fala mais alto", "abaixa o volume".
- **Chacoalhar:** ele fica tonto, com estrelinhas girando em volta da cabeça, e solta uma frase engraçada.
- **Carinho:** segure o dedo na tela e ele fica feliz e responde com uma frase fofa.
- **Dorme sozinho** depois de 30 s de silêncio. Dormindo, a conexão é fechada e não gasta API.

Chacoalhar ou fazer carinho com ele dormindo também acorda o Cozmo, já reagindo.

O app **AIChats** de fábrica (XiaoZhi) também ganhou o rosto do Cozmo no lugar do emoji.

## Material

- Waveshare ESP32-S3-Touch-AMOLED-1.75:
  - tela AMOLED 466×466 com toque;
  - 2 microfones e alto-falante;
  - acelerômetro QMI8658;
  - 16 MB de flash e 8 MB de PSRAM.
- Alto-falante pequeno no conector da placa. O que acompanha a placa serve.
- Bateria LiPo 3,7 V, opcional.
- Chaves de API:
  - [Deepgram](https://console.deepgram.com) para o Voice Agent;
  - [OpenAI](https://platform.openai.com) para o cérebro e a voz.

## Como compilar e gravar

Precisa do [ESP-IDF 5.5](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32s3/get-started/). Foi testado na 5.5.4.

1. Crie o seu arquivo de chaves. Ele fica fora do git:

   ```bash
   cp firmware/brookesia/components/Cozmo/secrets.example.h firmware/brookesia/components/Cozmo/secrets.h
   ```

   Preencha `OPENAI_KEY`, `DEEPGRAM_KEY` e a cidade do clima: `CIDADE`, `LAT` e `LON`.

2. Compile:

   ```bash
   idf.py -C firmware/brookesia set-target esp32s3 build
   ```

3. Grave tudo. Na primeira vez a gravação precisa ser completa, porque a tabela de partições mudou:

   ```bash
   idf.py -C firmware/brookesia -p /dev/cu.usbmodem101 flash
   ```

   Nas próximas vezes, se só o código mudou, basta gravar o app:

   ```bash
   python -m esptool --chip esp32s3 -p /dev/cu.usbmodem101 write_flash 0x200000 firmware/brookesia/build/esp32-s3-touch-amoled-brookesia.bin
   ```

4. Na placa, conecte o WiFi pelo app **Settings**.

> **Nunca publique o `.bin` compilado.** As suas chaves vão dentro dele.

## Como usar

| Gesto | O que acontece |
|---|---|
| Abrir o app **Cozmo** | Ele começa dormindo e mostra "Zzz... diga Hey Cozmo" |
| Dizer **"Hey Cozmo"** | Acorda, diz "Oi! Tô acordado." e a conversa começa |
| Toque rápido na tela | Dormindo: acorda. Falando: para de falar |
| Segurar o dedo na tela | Carinho: fica feliz e reage |
| Chacoalhar a placa | Fica tonto, com estrelinhas, e reage |
| 30 s de silêncio | Volta a dormir |

Exemplos do que dá para pedir: "que horas são?", "vai chover amanhã?", "fala mais alto", "fica bravo".

Enquanto ele fala, o microfone fica fechado para ele não ouvir a própria voz. Para interromper, toque na tela.

## Como funciona

```
microfones ──► AFE (cancela o eco, 16 kHz) ──┬─► dormindo: MultiNet procura "Hey Cozmo"
                                             └─► acordado: WebSocket ──► Deepgram Voice Agent
                                                                              │ Flux (ouve)
alto-falante ◄── voz 24 kHz ◄─────────────────────────────────────────────────┤ gpt-4o-mini (pensa)
rosto ◄── eventos e funções (set_expression, get_time, get_weather, set_volume) ┘ OpenAI tts-1 (fala)
```

| Arquivo | O que faz |
|---|---|
| `components/CozmoFace/` | Rosto: olhos, boca, piscadas, humores e tontura pelo acelerômetro |
| `components/Cozmo/Cozmo.cpp` | O app da tela inicial: toque, carinho e aviso de status |
| `components/Cozmo/CozmoVoz.cpp` | Áudio, wake word, conexão com o Deepgram e as funções (hora, clima, volume) |
| `components/XiaozhiApp/XiaozhiUi.cpp` | AIChats de fábrica com o rosto do Cozmo |
| `components/tcp_transport/` | Cópia do componente do ESP-IDF com uma correção (veja abaixo) |

## Ajustes

Tudo no começo de `CozmoVoz.cpp` e `CozmoFace.cpp`:

| Constante | Padrão | Para quê |
|---|---|---|
| `WAKE_WORDS` | "hey cozmo", "acorda cosmo"… | Frases que acordam o Cozmo. O modelo é em inglês, então várias grafias ajudam com o sotaque |
| `SONO_MS` | 30000 | Tempo de silêncio até ele dormir |
| `GANHO_VOZ` | 30 (3×) | Ganho da voz. Se distorcer, diminua |
| `TRANCO_MG` | 1300 | Força do chacoalhão para ele ficar tonto |
| `SETTINGS` | — | Prompt, voz, modelo e as funções que o agente pode chamar |

## Partições

O detector de "Hey Cozmo" (MultiNet7 em inglês, cerca de 2,6 MB) não cabia no espaço original. Por isso a tabela mudou:

| Partição | Início | Tamanho |
|---|---|---|
| factory (app) | 0x200000 | 7,25 MB |
| model (voz) | 0x940000 | 3,1 MB |
| storage (fontes) | 0xC60000 | 3,6 MB |

O espaço entre 0x110000 e 0x200000 ficou sem uso. O NVS, onde ficam o WiFi e o vínculo com o xiaozhi.me, não muda de lugar.

## Armadilhas que já foram resolvidas

Para quem for mexer, estas custaram tempo:

- **TLS falhava com `-0x4290`.** A RAM interna não basta para verificar o certificado RSA 4096 do Deepgram. Solução: `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y`, que põe o TLS na PSRAM.
- **Deepgram respondia 404 no handshake.** O cliente WebSocket do ESP-IDF sempre manda `Host: agent.deepgram.com:443`, e o Deepgram só aceita sem a porta. A cópia em `components/tcp_transport` omite a porta quando ela é 80 ou 443.
- **A voz da resposta não tocava.** Com o Flux, o Deepgram não manda `AgentStartedSpeaking`. A fala é liberada no `EndOfTurn`.
- **A conexão caía com WiFi fraco.** Subir áudio enquanto a voz desce travava o envio. Agora:
  - enquanto ele fala, só vai `KeepAlive`;
  - a economia de energia do WiFi fica desligada enquanto ele está acordado;
  - se a conexão cair, ele reconecta uma vez sozinho.
- **Watchdog no microfone.** O cancelamento de eco roda dentro do `feed()` e precisa de pilha na RAM interna, no núcleo 0.
- **Fuso horário.** O firmware de fábrica vinha com o fuso da China (`CST-8`). Agora usa `<-03>3`, Brasília.

## Custos

Acordado, cada minuto de conversa consome Deepgram (Voice Agent) e OpenAI (`gpt-4o-mini` e `tts-1`).
Dormindo, a conexão fica fechada e não há custo.

## Créditos e licença

- Firmware base: [Waveshare ESP32-S3-Touch-AMOLED-1.75](https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.75)
  (Apache 2.0). O primeiro commit deste repositório é o original, sem alterações. As mudanças estão no segundo.
- O `tcp_transport` é uma cópia do ESP-IDF (Apache 2.0) com a correção do `Host`.
- O processamento de áudio vem do AIChats, que é derivado do [xiaozhi-esp32](https://github.com/78/xiaozhi-esp32) (MIT).
- Rosto, voz e app Cozmo: [@fabriciowth](https://github.com/fabriciowth), sob Apache 2.0.

Versão original com OLED e ESP32: [cozmo-esp32](https://github.com/fabriciowth/cozmo-esp32).
