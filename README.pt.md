# RC-80 – Rádio toca-fitas anos 80 (firmware Arduino)

Firmware do **RC-80**, um rádio toca-fitas no estilo anos 80, impresso em 3D. Tudo é comandado por um Arduino Nano:
- rádio AM/FM estéreo;
- mostrador analógico com agulha de verdade;
- toca-fitas com auto-reverse;
- bateria interna recarregável.

**Modelo 3D, arquivos de impressão, manual de montagem e lista de compras:**
👉 **[RC-80 no MakerWorld](https://makerworld.com/en/models/3479422-rc-80-80s-boombox-radio-cassette-player)**

Este repositório contém apenas o código-fonte do firmware.

🇺🇸 [Read in English](README.md)

---

## Sketches

| Pasta | Descrição |
|---|---|
| [`firmware/radio_k7`](firmware/radio_k7/radio_k7.ino) | Comentários em português. Ajustado para o Brasil (AM com passo de 10 kHz, FM com deênfase de 75 µs). |
| [`firmware/radio_k7_en`](firmware/radio_k7_en/radio_k7_en.ino) | Comentários em inglês. Tem também os ajustes regionais `AM_STEP` e `FM_DEEMPH`. |

Os dois têm a mesma lógica. Grave **apenas um**.

## O que o firmware faz

- **Sintonia analógica:** o botão TUNING move a agulha do mostrador por um cordão. O mesmo cordão gira um potenciômetro de 10 voltas (Bourns 3590S).
  - O Nano lê a posição da agulha em A0, filtra a leitura e sintoniza o Si4732 exatamente na frequência que a agulha aponta.
  - No FM a escala é linear, de 88 a 108 MHz. No AM é logarítmica, de 530 a 1600 kHz, igual à escala impressa.
- **Chave BAND:** escolhe AM ou FM. O LED **FM STEREO** acende quando a estação é estéreo.
- **Toca-fitas:** comanda um mecanismo lógico **CRP42602Y** (motor de 12 V, solenoide de 9 V).
  - Teclas: PLAY, STOP, F.FWD, REW, PAUSE e A-B (troca de lado).
  - **Auto-reverse:** no fim do lado A ele toca o lado B; no fim do lado B ele para.
  - **Fim da fita:** detectado pelo sensor de rotação dos carretéis.
- **Troca de áudio:** um relé passa de rádio para fita e de volta sozinho. O amplificador fica mudo durante a troca para não estalar.
- **Abertura da porta:** com a fita parada, **STOP/EJ** faz um servo SG90 soltar a trava da porta.
- **Bateria:** um divisor em A6 mede o pacote Li-ion 3S.
  - Abaixo de 9,9 V o LED pisca: é hora de carregar.
  - Abaixo de 9,3 V a fita para e o som é cortado.
- **Economia:** o motor desliga quando não há fita, ou quando o toca-fitas fica 30 s parado.

## Hardware (resumo)

| Função | Peça |
|---|---|
| Microcontrolador | Arduino Nano (ATmega328P, 5 V) |
| Rádio | Si4732-A10 (I2C, 3,3 V, com conversor de nível BSS138) |
| Mecanismo de fita | CRP42602Y / M02753900D (lógico, auto-reverse) |
| Pré da cabeça | TA7668 |
| Amplificador | PAM8610 2 × 10 W |
| Volume + liga/desliga | Potenciômetro duplo 10k log com chave (Adafruit #3482) |
| Sensor da sintonia | Bourns 3590S-2-103L, 10 kΩ, 10 voltas |
| Trava da porta | Micro servo SG90 |
| Energia | 3 × 18650 (3S) + BMS, jack de carga 12,6 V, MP1584 5 V, XL6009 14 V, LM317 12 V / 9 V |

Os esquemas completos, a tabela de ligações e a lista de compras estão no manual, no MakerWorld.

## Pinagem (Arduino Nano)

| Pino | Ligação | Observação |
|---|---|---|
| D2 | CRP42602Y pino 2 – ROTATION_SENS | interrupção, pull-up de 10k |
| D3 | Sinal do servo SG90 | |
| D4 | CRP42602Y pino 3 – GEAR_STATUS_SW | LOW = engrenagem em função |
| D5 | CRP42602Y pino 7 – CASSETTE_DETECT_SW | LOW = tem fita |
| D6 | Driver do solenoide (gate do IRLZ44N) | HIGH = puxa |
| D7 | Chave do motor 12 V (BC337 → IRF9540N) | HIGH = motor ligado |
| D8 | Relé de áudio (2N2222) | HIGH = fita, LOW = rádio |
| D9 | MUTE do PAM8610 | ver `AMP_MUTE_LEVEL` |
| D10 | LED FM STEREO / bateria fraca | 1 kΩ |
| D12 | RESET do Si4732 | pelo conversor de nível |
| A0 | Cursor do pot. de sintonia | 100 nF para GND |
| A1 | Microchave BAND | fechada = FM |
| A4 / A5 | I2C SDA / SCL → Si4732 | pelo conversor de nível |
| A6 | Divisor da bateria 100k / 33k | |
| A7 | Teclas (escada de resistores, pull-up 10k) | |

### Escada das teclas (A7)

| Tecla | Resistor para GND | Valor no ADC |
|---|---|---|
| STOP/EJ | 0 Ω | 0 |
| PLAY | 1 kΩ | 93 |
| REW | 2,2 kΩ | 184 |
| F.FWD | 3,9 kΩ | 288 |
| PAUSE | 6,8 kΩ | 414 |
| A-B | 15 kΩ | 614 |

### Conector CN1 do CRP42602Y

| Pino | Sinal | Ligação |
|---|---|---|
| 1 | REC_B_SW | não usado |
| 2 | ROTATION_SENS | D2 |
| 3 | GEAR_STATUS_SW | D4 |
| 4 | SOLENOID_1 | dreno do IRLZ44N |
| 5 | SOLENOID_2 | +9 V |
| 6 | GND | GND |
| 7 | CASSETTE_DETECT_SW | D5 |
| 8 | P12V | +12 V chaveado (motor + LED do sensor) |
| 9 | REC_A_SW | não usado |

## Como compilar e gravar

1. Instale o **Arduino IDE** (ou o `arduino-cli`).
2. Instale as bibliotecas pelo gerenciador de bibliotecas:
   - **PU2CLR SI4735**, de Ricardo Lima Caratti;
   - **Servo**.

   A **EEPROM** e a **Wire** já vêm com o núcleo AVR.
3. Abra `firmware/radio_k7/radio_k7.ino`.
4. Escolha a placa **Arduino Nano**. Em clones antigos, use o processador *ATmega328P (Old Bootloader)*.
5. Envie o sketch. Ele ocupa cerca de 11 KB da memória.

Com o `arduino-cli`:

```sh
arduino-cli lib install "PU2CLR SI4735" Servo
arduino-cli compile -b arduino:avr:nano firmware/radio_k7
arduino-cli upload  -b arduino:avr:nano -p COM3 firmware/radio_k7
```

## Ajustes

Todos os ajustes ficam no começo do sketch:

| Constante | Padrão | Para que serve |
|---|---|---|
| `AMP_MUTE_LEVEL` | `HIGH` | Nível que silencia o amplificador. Algumas placas PAM8610 silenciam com LOW. |
| `SERVO_REST` / `SERVO_RELEASE` | 20° / 75° | Ângulos do servo: porta travada e porta solta |
| `VBAT_LOW` / `VBAT_EMPTY` | 9,9 V / 9,3 V | Aviso de bateria fraca e corte |
| `MOTOR_IDLE_OFF_MS` | 30000 | Desliga o motor depois desse tempo parado |
| `AM_STEP`, `AM_MIN`, `AM_MAX` *(sketch EN)* | 10, 530, 1600 | Faixa de AM. Na Europa, Ásia, África e Oceania use 9, 531 e 1602. |
| `FM_DEEMPH` *(sketch EN)* | 2 (75 µs) | Use 1 (50 µs) fora das Américas |

## Calibração da escala (uma vez)

1. Com o rádio desligado, segure **STOP/EJ** e ligue no VOLUME.
2. Com o LED piscando devagar, leve a agulha exatamente até **88** e aperte **PLAY**.
3. Com o LED piscando rápido, leve a agulha até **108** e aperte **PLAY**.

Os dois pontos ficam gravados na EEPROM. A escala de AM sai da mesma calibração.

## Créditos

- **Tempos do solenoide e pinagem do CRP42602Y:** baseados em [elehobica/pico_crp42602y_ctrl](https://github.com/elehobica/pico_crp42602y_ctrl) (BSD-2-Clause).
- **Referência de mecanismos lógicos:** [Fagear/AVRTapeControl](https://github.com/Fagear/AVRTapeControl).
- **Biblioteca do rádio:** [PU2CLR SI4735](https://github.com/pu2clr/SI4735), de Ricardo Lima Caratti.

## Licença

O firmware é distribuído sob a [licença MIT](LICENSE).
