#pragma once

#include "httpheader.h"

#include <util/system/compat.h>
#include <library/cpp/http/misc/httpcodes.h> 

class httpDigestHandler {
protected:
    const char* User_;
    const char* Password_;
    char* Nonce_;
    int NonceCount_;
    char* HeaderInstruction_;

    void clear();

    void generateCNonce(char* outCNonce);

    void digestCalcHA1(const THttpAuthHeader& hd,
                       char* outSessionKey,
                       char* outCNonce);

    void digestCalcResponse(const THttpAuthHeader& hd,
                            const char* method,
                            const char* path,
                            const char* nonceCount,
                            char* outResponse,
                            char* outCNonce);

public:
    httpDigestHandler();
    ~httpDigestHandler();

    void setAuthorization(const char* user,
                          const char* password);
    bool processHeader(const THttpAuthHeader* header,
                       const char* path,
                       const char* method,
                       const char* cnonce = nullptr);

    bool empty() const {
        return (!User_);
    }

    const char* getHeaderInstruction() const;
};
