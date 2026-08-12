
#include "MimeTypes.h"

void setup() {
  Serial.begin(115200);
}

void loop() {
  Serial.println();
  Serial.println("Testing getType()");
  Serial.print("Empty string: ");
  Serial.println(MimeTypes::getType(""));

  Serial.print("Just dot: ");
  Serial.println(MimeTypes::getType("."));

  Serial.print("Dot as the last char: ");
  Serial.println(MimeTypes::getType("txt."));

  Serial.print("No dot in extension: ");
  Serial.println(MimeTypes::getType("pdf"));

  Serial.print("Dot in extension: ");
  Serial.println(MimeTypes::getType(".txt"));

  Serial.print("Full path: ");
  Serial.println(MimeTypes::getType("/images/image.png"));

  Serial.print("Mixed case: ");
  Serial.println(MimeTypes::getType("/bin/binary.BiN"));
  Serial.println();


  Serial.println("Testing getExtension()");
  Serial.print("Empty string: ");
  Serial.println(MimeTypes::getExtension(""));
  Serial.print("Not found: ");
  Serial.println(MimeTypes::getExtension("not/found"));

  Serial.print("First of application/pdf: ");
  Serial.println(MimeTypes::getExtension("application/pdf"));

  Serial.print("All of text/plain: ");
  const char * type;
  int i = 0;
  do  {
    type = MimeTypes::getExtension("text/plain", i++);
    Serial.print(type);
    Serial.print(" ");
  } while (type != NULL);
  Serial.println();

  delay(5000);
}
