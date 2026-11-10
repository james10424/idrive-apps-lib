// SPDX-License-Identifier: MPL-2.0
// Copyright (C) 2026 Robert Nisipeanu
// Declares the API of RAMSES (https://github.com/bmwcarit/ramses), BMW Car IT GmbH, MPL-2.0.
// Framework, object base classes and value-object storage for libramses 19.0.14.
#ifndef RAMSES19_FRAMEWORK_H
#define RAMSES19_FRAMEWORK_H

#include "types.h"

// Objects the library allocates (scenes, nodes, resources ...) are only ever
// used through pointers and references; their declarations carry no data.
#define RAMSES19_LIBRARY_OWNED(T) \
    T() = delete;                 \
    T(const T&) = delete;         \
    T& operator=(const T&) = delete; \
    ~T() = delete

// Objects the caller constructs call the library's own constructor and
// destructor; the storage is larger than the library's object (24 bytes for
// every StatusObject-derived value class, 0x20 for RamsesClient).
#define RAMSES19_VALUE_STORAGE(T)    \
    T(const T&) = delete;            \
    T& operator=(const T&) = delete; \
private:                             \
    alignas(8) unsigned char m_storage[64]

namespace ramses
{
    class StatusObject
    {
    public:
        status_t validate() const;
    protected:
        StatusObject() = delete;
        StatusObject(const StatusObject&) = delete;
        // Exported (D2); releases the implementation object. Value classes
        // whose destructor is inline in 19 chain to it.
        ~StatusObject();
    };

    class RamsesObject : public StatusObject
    {
    public:
        const char* getName() const;
        status_t setName(const char* name);
        ERamsesObjectType getType() const;
        // The object's type or one of its bases.
        bool isOfType(ERamsesObjectType type) const;
        RAMSES19_LIBRARY_OWNED(RamsesObject);
    };

    class RamsesFrameworkConfig : public StatusObject
    {
    public:
        RamsesFrameworkConfig(int32_t argc, const char* const* argv);
        ~RamsesFrameworkConfig();
        // Takes an ERamsesCommunicationUser value; see ABI.md before choosing one.
        status_t enableSomeIPCommunication(uint32_t communicationUser);
        void setDLTApplicationID(const char* id);
        void setDLTApplicationDescription(const char* description);
        void setPeriodicLogsEnabled(bool enabled);
        const char* getDLTApplicationID() const;
        RAMSES19_VALUE_STORAGE(RamsesFrameworkConfig);
    };

    class RamsesFramework
    {
    public:
        explicit RamsesFramework(const RamsesFrameworkConfig& config);
        RamsesFramework(int32_t argc, const char** argv);
        ~RamsesFramework();
        status_t connect();
        bool isConnected() const;
        status_t disconnect();
        RAMSES19_VALUE_STORAGE(RamsesFramework);
    };
}

#endif
