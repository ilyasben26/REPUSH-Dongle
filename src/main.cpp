#include <Arduino.h>

int led = LED_BUILTIN;

int value = 0;

void setup()
{
  Serial.begin(9600);    // PC <-> Arduino
  Serial1.begin(115200); // Arduino <-> FPGA. RX: PB23, TX: PB22
  pinMode(led, OUTPUT);
}

void loop()
{
  if (Serial.available())
  {
    value = Serial.read();
    delay(5);
    if (value == '1')
    {
      digitalWrite(led, HIGH);
      Serial.println("LED ON");
    }

    if (value == '0')
    {
      digitalWrite(led, LOW);
      Serial.println("LED OFF");
    }
  }
}
