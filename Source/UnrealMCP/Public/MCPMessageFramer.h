#pragma once

#include "CoreMinimal.h"

// 按字节识别 JSON 对象边界，兼容旧版无换行请求和换行分隔请求。
class FMCPMessageFramer
{
public:
    static constexpr int32 MaxMessageBytes = 1024 * 1024;

    bool Push(uint8 Byte, FString& OutMessage)
    {
        OutMessage.Reset();
        if (Buffer.Num() == 0)
        {
            if (Byte == ' ' || Byte == '\t' || Byte == '\r' || Byte == '\n')
            {
                return true;
            }
            if (Byte != '{')
            {
                return false;
            }
        }
        if (Buffer.Num() >= MaxMessageBytes)
        {
            return false;
        }
        Buffer.Add(Byte);
        if (bInString)
        {
            if (bEscaped)
            {
                bEscaped = false;
            }
            else if (Byte == '\\')
            {
                bEscaped = true;
            }
            else if (Byte == '"')
            {
                bInString = false;
            }
        }
        else if (Byte == '"')
        {
            bInString = true;
        }
        else if (Byte == '{' || Byte == '[')
        {
            if (++Depth > 128)
            {
                return false;
            }
        }
        else if (Byte == '}' || Byte == ']')
        {
            if (--Depth == 0)
            {
                const FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Buffer.GetData()), Buffer.Num());
                OutMessage = FString(Text.Length(), Text.Get());
                Buffer.Reset();
            }
        }
        return true;
    }

private:
    TArray<uint8> Buffer;
    int32 Depth = 0;
    bool bInString = false;
    bool bEscaped = false;
};
