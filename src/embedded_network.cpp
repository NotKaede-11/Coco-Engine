#include "../third_party/ravager/nnue/incbin.h"

// The build runs from the repository root on Windows, Linux and macOS.
// incbin stores the original bytes directly, without a generated 450 MB hex header.
INCBIN(CocoNetwork, "networks/coco.nnue");

extern "C" const unsigned char* coco_network_data() { return gCocoNetworkData; }
extern "C" unsigned int coco_network_size() { return gCocoNetworkSize; }
