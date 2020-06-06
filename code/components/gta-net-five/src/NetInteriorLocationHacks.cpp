#include <StdInc.h>
#include <Hooking.h>

#include <Pool.h>
#include <rlNetBuffer.h>

#include <ICoreGameInit.h>
#include <Streaming.h>

#include <Error.h>

//
// fwInteriorLocation is somehow directly serialized over the network, which breaks if interior proxies are not equally
// ordered across peers. This is a set of hacks to (compatibly!) change fwInteriorLocation network serialization to pass
// an interior proxy hash instead of a raw interior proxy pool index.
//

namespace rage
{
class fwInteriorLocation
{
public:
	inline fwInteriorLocation()
	{
		m_interiorIndex = -1;
		m_isPortal = false;
		m_unk = false;
		m_innerIndex = -1;
	}

	inline fwInteriorLocation(uint16_t interiorIndex, bool isPortal, uint16_t innerIndex)
		: fwInteriorLocation()
	{
		m_interiorIndex = interiorIndex;
		m_isPortal = isPortal;
		m_innerIndex = innerIndex;
	}

	inline uint16_t GetInteriorIndex()
	{
		return m_interiorIndex;
	}

	inline uint16_t GetRoomIndex()
	{
		assert(!m_isPortal);

		return m_innerIndex;
	}

	inline uint16_t GetPortalIndex()
	{
		assert(m_isPortal);

		return m_innerIndex;
	}

	inline bool IsPortal()
	{
		return m_isPortal;
	}

private:
	uint16_t m_interiorIndex;
	uint16_t m_isPortal : 1;
	uint16_t m_unk : 1;
	uint16_t m_innerIndex : 14;
};
}

class CInteriorProxy
{
public:
	virtual ~CInteriorProxy() = 0;

	uint32_t mapIndex; // 8
	uint32_t pad; // 12
	uint32_t occlusionIndex; // 16
	uint16_t unkFlag; // 20
	uint8_t pad2[112 - 24]; // 24
	float position[4];
	uint8_t pad3[100];
	uint32_t archetypeHash;

	inline uint32_t GetCustomHash()
	{
		static auto store = streaming::Manager::GetInstance()->moduleMgr.GetStreamingModule("ymap");
		auto pool = ((atPoolBase*)((char*)store + 56));

		auto entry = pool->GetAt<char>(mapIndex);

		uint32_t mapHash = 0;

		if (entry)
		{
			mapHash = *(uint32_t*)(entry + 12);
		}

		return (archetypeHash + mapHash + (int)floor(position[0] * 100.0) + (int)floor(position[1] * 100.0) + (int)floor(position[2] * 100.0));
	}
};

class CInteriorInst
{
public:
	static CInteriorProxy* GetInteriorForLocation(const rage::fwInteriorLocation& location);
};

static hook::cdecl_stub<CInteriorProxy*(const rage::fwInteriorLocation&)> _getInteriorForLocation([]()
{
	return hook::get_pattern("45 3B 48 10 7D 33 49  8B 40 08", -0x1D);
});

CInteriorProxy* CInteriorInst::GetInteriorForLocation(const rage::fwInteriorLocation& location)
{
	return _getInteriorForLocation(location);
}


namespace rage
{
struct CSyncDataBase
{
	virtual ~CSyncDataBase() = 0;

	// remember: MSVC ABI vtable order for overloads is *reverse* declaration order
	// the overloads therefore are the reverse order in memory/IDA
	virtual void SerialiseBitField(int8_t& val, int len, const char* name) = 0;
	virtual void SerialiseBitField(int16_t& val, int len, const char* name) = 0;
	virtual void SerialiseBitField(int32_t& val, int len, const char* name) = 0;
	virtual void SerialiseBitField(uint8_t& val, int len, const char* name) = 0;
	virtual void SerialiseBitField(uint16_t& val, int len, const char* name) = 0;
	virtual void SerialiseBitField(uint32_t& val, int len, const char* name) = 0;
	virtual void SerialiseBool(bool& val, int len, const char* name) = 0;
	virtual void SerialiseInteger(int8_t& val, int len, const char* name) = 0;
	virtual void SerialiseInteger(int16_t& val, int len, const char* name) = 0;
	virtual void SerialiseInteger(int32_t& val, int len, const char* name) = 0;
	virtual void SerialiseInteger(int64_t& val, int len, const char* name) = 0;
	virtual void SerialiseUnsigned(uint8_t& val, int len, const char* name) = 0;
	virtual void SerialiseUnsigned(uint16_t& val, int len, const char* name) = 0;
	virtual void SerialiseUnsigned(uint32_t& val, int len, const char* name) = 0;
	virtual void SerialiseUnsigned(uint64_t& val, int len, const char* name) = 0;
	virtual void SerialisePackedFloat(float& val, float range, int len, const char* name) = 0;
	virtual void SerialisePackedUnsignedFloat(float& val, float range, int len, const char* name) = 0;
	virtual void SerialiseObjectID(uint16_t& val, const char* name) = 0;
	virtual void SerialisePosition(float* vec, const char* name, uint32_t len) = 0;
	virtual void SerialiseOrientation(float* mat, const char* name) = 0;
	virtual void SerialiseVector(float* vec, float range, int len, const char* name) = 0;
	virtual void SerialiseQuaternion(float* quat, int len, const char* name) = 0;
	virtual void SerialiseDataBlock(uint8_t* data, int len, const char* name) = 0;
	virtual void SerialiseString(char* data, int len, const char* name) = 0;
	virtual bool GetIsMaximumSizeSerialiser() = 0;
	virtual uint32_t GetSize() = 0;
};
}

static uintptr_t g_writerVtbl;
static uintptr_t g_loggerVtbl;

static void CSyncDataBase__Serialise_CDynamicEntityGameStateDataNode(rage::CSyncDataBase* self, rage::fwInteriorLocation* location/*, int length, const char* name*/)
{
	bool isWriter = false;

	if (*(uintptr_t*)self == g_writerVtbl)
	{
		isWriter = true;
	}
	else if (*(uintptr_t*)self == g_loggerVtbl)
	{
		isWriter = true;
	}
	else if (self->GetIsMaximumSizeSerialiser())
	{
		isWriter = true;
	}

	if (!isWriter)
	{
		uint32_t value = 0;
		self->SerialiseUnsigned(value, 32, "Interior Flags");

		// is this a new-format location?
		if ((value >> 29) == 0b010 && (value & 0xFFFF) == 0xF5F5)
		{
			value &= ~0xF0000000;
			value >>= 8;

			uint32_t interiorHash;
			self->SerialiseUnsigned(interiorHash, 32, "Interior Hash");

			AddCrashometry("new_interior_location_format_seen", "true");

			if (interiorHash != -1)
			{
				static auto interiorPool = rage::GetPoolBase("InteriorProxy");
				for (int i = 0; i < interiorPool->GetCountDirect(); i++)
				{
					auto interiorProxy = interiorPool->GetAt<CInteriorProxy>(i);

					if (interiorProxy)
					{
						if (interiorProxy->GetCustomHash() == interiorHash)
						{
							rage::fwInteriorLocation loc{uint16_t(i), (value & 1) ? true : false, uint16_t(value >> 1)};
							*location = loc;

							break;
						}
					}
				}
			}
			else
			{
				*location = {};
			}
		}
		else
		{
			*location = *(rage::fwInteriorLocation*)&value;
		}
	}
	else
	{
		static auto icgi = Instance<ICoreGameInit>::Get();
		
		bool useNew = false;

#ifdef _DEBUG
		useNew = true;
#endif

		std::string policyVal;
		if (icgi->GetData("policy", &policyVal))
		{
			if (policyVal.find("[new_interior_hash]") != std::string::npos)
			{
				useNew = true;
			}
		}

		if (!useNew)
		{
			self->SerialiseUnsigned(*(uint32_t*)location, 32, "Interior Flags (Legacy)");
		}
		else
		{
			AddCrashometry("new_interior_location_format_sent", "true");

			uint32_t value1 = 0;
			uint32_t value2 = 0;

			auto interior = CInteriorInst::GetInteriorForLocation(*location);

			if (interior && location->GetInteriorIndex() != 0xFFFF)
			{
				if (location->IsPortal())
				{
					value1 |= 1;
					value1 |= location->GetPortalIndex() << 1;
				}
				else
				{
					value1 |= location->GetRoomIndex() << 1;
				}

				value1 <<= 8;
				value1 |= 0x4000F5F5;

				if (interior)
				{
					value2 = interior->GetCustomHash();
				}
			}
			else
			{
				value1 = 0x4000F5F5;
				value2 = 0xFFFFFFFF;
			}

			self->SerialiseUnsigned(value1, 32, "Interior Flags");
			self->SerialiseUnsigned(value2, 32, "Interior Hash");
		}
	}
}

static HookFunction hookFunction([]()
{
	g_writerVtbl = hook::get_address<uintptr_t>(hook::get_pattern("4C 8B 10 48 8B C8 41 FF 52 08 48 8B 4B 08", 0x11));
	g_loggerVtbl = hook::get_address<uintptr_t>(hook::get_pattern("48 8B 90 10 27 00 00 48 8D 05 ? ? ? ? 49 89 53 E8", 10));

	// CDynamicEntityGameStateDataNode Serialize
	auto location = hook::get_pattern("45 8D 41 20 FF 50 68 48 8B 07 49 8D 97 C4");
	hook::nop(location, 7);
	hook::call(location, CSyncDataBase__Serialise_CDynamicEntityGameStateDataNode);
});
