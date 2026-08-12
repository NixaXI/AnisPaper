# MimeTypes
C++ content-type utility class.

## API
### static const char* getType(const char * path)
Lookup the content-type associated with a file path or extension.
```cpp
MimeTypes::getType("pdf");
MimeTypes::getType(".txt");
MimeTypes::getType("/images/image.png");
MimeTypes::getType("/bin/binary.BiN");
```


### static const char* getExtension(const char * type, int skip = 0)
Get the extension for a content-type.
```cpp
MimeTypes::getExtension("application/pdf");

const char * type;
int i = 0;
do  {
  type = MimeTypes::getExtension("text/plain", i++);
} while (type != NULL);
```

## Mime data source
[node-mime lite version](https://github.com/broofa/node-mime#lite-version)
