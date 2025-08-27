#pragma once

namespace duckdb {

class BinaryReader {
public:
	BinaryReader(const char* data, size_t size)
	    : beg(data), ptr(data), end(data + size) {}

	void Rollback(size_t size) {
		if (ptr - size < beg) {
			throw InvalidInputException("BinaryReader: Attempt to rollback past beginning of buffer");
		}
		ptr -= size;
	}

	bool IsAligned(size_t alignment = alignof(double)) const {
		return (reinterpret_cast<uintptr_t>(ptr) % alignment) == 0;
	}

	const char* Reserve(size_t size) {
		if (ptr + size > end) {
			throw InvalidInputException("BinaryReader: Attempt to reserve past end of buffer");
		}
		const char* result = ptr;
		ptr += size;
		return result;
	}

	void Skip(size_t size) {
		if (ptr + size > end) {
			throw InvalidInputException("BinaryReader: Attempt to skip past end of buffer");
		}
		ptr += size;
	}

	template<class T, bool IS_BIG_ENDIAN = false>
	T Read() {
		if (ptr + sizeof(T) > end) {
			throw InvalidInputException("BinaryReader: Attempt to read past end of buffer");
		}

		T value;

		if (!IS_BIG_ENDIAN) {
			memcpy(&value, ptr, sizeof(T));
		} else {
			// Handle big-endian reading
			char buffer[sizeof(T)];
			for (size_t i = 0; i < sizeof(T); i++) {
				buffer[i] = ptr[sizeof(T) - 1 - i];
			}
			memcpy(&value, buffer, sizeof(T));
		}

		ptr += sizeof(T);

		return value;
	}

	template<class T>
	T Read(bool is_big_endian) {
		return is_big_endian ? Read<T, true>() : Read<T, false>();
	}

	bool IsAtEnd() const {
		return ptr >= end;
	}

	void Reset() {
		ptr = beg;
	}

private:
	const char* beg;
	const char* ptr;
	const char* end;
};

class BinaryWriter {
public:
	template<class T>
	void Write(const T& value) {
		static_assert(std::is_trivially_copyable<T>::value, "Type must be trivially copyable");
		const auto offset = buffer.size();
		buffer.resize(buffer.size() + sizeof(T));
		memcpy(buffer.data() + offset, &value, sizeof(T));
	}

	void Reserve(size_t size) {
		buffer.reserve(buffer.size() + size);
	}

	void Copy(const char* data, size_t size) {
		buffer.insert(buffer.end(), data, data + size);
	}

	template<class T>
	void CopyTemplated(const char* data, size_t count) {
		// We're allocating into the vector anyway
		Copy(data, sizeof(T) * count);
	}

	void Reset() {
		buffer.clear();
	}
	const char* GetData() const {
		return buffer.data();
	}
	size_t GetSize() const {
		return buffer.size();
	}

	size_t GetPosition() const {
		return buffer.size();
	}

private:
	vector<char> buffer;
};

template<bool SAFE = true>
class FixedBinaryWriter {
public:
	FixedBinaryWriter(char* data, size_t size)
	    : beg(data), ptr(beg), end(beg + size) {
	}

	template<class T>
	void Write(const T &value) {
		if (SAFE && ptr + sizeof(T) > end) {
			throw InvalidInputException("FixedBinaryWriter: Attempt to write past end of buffer");
		}
		memcpy(ptr, &value, sizeof(T));
		ptr += sizeof(T);
	}

	void Copy(const char* data, size_t size) {
		if (SAFE && ptr + size > end) {
			throw InvalidInputException("FixedBinaryWriter: Attempt to copy past end of buffer");
		}
		memcpy(ptr, data, size);
		ptr += size;
	}

	template<class T>
	void CopyTemplated(const char* data, size_t count) {
		if (SAFE && ptr + sizeof(T) * count > end) {
			throw InvalidInputException("FixedBinaryWriter: Attempt to copy past end of buffer");
		}
		for (size_t i = 0; i < count; i++) {
			memcpy(ptr + sizeof(T) * i, data + sizeof(T) * i, sizeof(T));
		}
		ptr += sizeof(T) * count;
	}

	size_t GetPosition() const {
		return ptr - beg;
	}

	void Skip(size_t size) {
		if (SAFE && ptr + size > end) {
			throw InvalidInputException("FixedBinaryWriter: Attempt to skip past end of buffer");
		}
		ptr += size;
	}

private:
	char* beg;
	char* ptr;
	char* end;
};



// Geometry Type Enum
enum class GeometryType : uint8_t {
	INVALID = 0,
	POINT = 1,
	LINESTRING = 2,
	POLYGON = 3,
	MULTIPOINT = 4,
	MULTILINESTRING = 5,
	MULTIPOLYGON = 6,
	GEOMETRYCOLLECTION = 7,
};

enum class VertexType : uint8_t {
	XY = 0,
	XYZ = 1,
	XYM = 2,
	XYZM = 3,
};

struct VertexXY {
	static constexpr auto TYPE = VertexType::XY;
	static constexpr auto HAS_Z = false;
	static constexpr auto HAS_M = false;
	double x;
	double y;
};

struct VertexXYZ {
	static constexpr auto TYPE = VertexType::XYZ;
	static constexpr auto HAS_Z = true;
	static constexpr auto HAS_M = false;
	double x;
	double y;
	double z;
};

struct VertexXYM {
	static constexpr auto TYPE = VertexType::XYM;
	static constexpr auto HAS_Z = false;
	static constexpr auto HAS_M = true;
	double x;
	double y;
	double m;

};

struct VertexXYZM {
	static constexpr auto TYPE = VertexType::XYZM;
	static constexpr auto HAS_Z = true;
	static constexpr auto HAS_M = true;
	double x;
	double y;
	double z;
	double m;
};

} // namespace duckdb