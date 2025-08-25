#define DUCKDB_EXTENSION_MAIN

#include "geobench_extension.hpp"
#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/extension_util.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"

//======================================================================================================================
// Common
//======================================================================================================================
namespace duckdb {
namespace {

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

struct BKBMeta {
	uint8_t meta[4];
	uint32_t size;

	void Verify() const {
		if (meta[0] != 0x02 || meta[1] != 0x01) {
			throw InvalidInputException("BKBMeta: Invalid BKB header");
		}
	}

	GeometryType GetType() const {
		return static_cast<GeometryType>(meta[3]);
	}
	bool HasZ() const {
		return (meta[2] & 0x01) != 0;
	}
	bool HasM() const {
		return (meta[2] & 0x02) != 0;
	}
	uint32_t GetCount() const { return size; }
};

struct WKBMeta {
	char bytes[5];
	uint8_t LE() const { return bytes[0]; }
	uint32_t GetType() const {
		uint32_t type = 0;
		memcpy(&type, bytes + 1, 4);
		return type;
	}
	void SetLE(uint8_t le) { bytes[0] = le; }
	void SetType(uint32_t type) { memcpy(bytes + 1, &type, 4); }
};


} // namespace
} // namespace duckdb
//======================================================================================================================
// Types
//======================================================================================================================
namespace duckdb {
namespace {

struct Types {

	// "Well Known Binary" (WKB) Geometry Type
	static LogicalType WKB() {
		auto ltype = LogicalType(LogicalTypeId::BLOB);
		ltype.SetAlias("WKB");
		return ltype;
	}

	// "Better Known Binary" (BKB) Geometry Type
	static LogicalType BKB() {
		auto ltype = LogicalType(LogicalTypeId::BLOB);
		ltype.SetAlias("BKB");
		return ltype;
	}

	static void Register(DatabaseInstance &db) {
		ExtensionUtil::RegisterType(db, "WKB", WKB());
		ExtensionUtil::RegisterType(db, "BKB", BKB());
	}
};

} // namespace
} // namespace duckdb
//======================================================================================================================
// Visitor
//======================================================================================================================
namespace duckdb {
namespace {

struct Tags {
	struct Any {};

	struct Simple : Any {};
	struct Complex : Any {};

	struct Point : Simple {};
	struct Line : Simple {};
	struct Ring : Simple {};

	struct Polygon : Complex {};
	struct Multi : Complex {};

	struct MultiPolygon : Multi {};
	struct MultiPoint : Multi {};
	struct MultiLine : Multi {};
	struct GeometryCollection : Multi {};
};

template<class IMPL>
class GeometryVisitor {
private:
	template<class VERTEX_TYPE = VertexXY, bool IS_BIG_ENDIAN>
	void VisitWKBInternal(BinaryReader &reader, GeometryType type) {
		switch (type) {
			case GeometryType::POINT: {

				constexpr auto nan = std::numeric_limits<double>::quiet_NaN();
				constexpr double nan_array[] = {nan, nan, nan, nan};

				auto reader_copy = reader;

				const auto vertex_array = reader.Reserve(sizeof(VERTEX_TYPE));
				const auto vertex_count = memcmp(vertex_array, &nan_array, sizeof(VERTEX_TYPE)) == 0 ? 0 : 1;

				static_cast<IMPL*>(this)->template Enter<VERTEX_TYPE>(Tags::Point{}, vertex_count);
				static_cast<IMPL*>(this)->template Vertices<VERTEX_TYPE, IS_BIG_ENDIAN>(reader_copy, vertex_count);
				static_cast<IMPL*>(this)->template Leave<VERTEX_TYPE>(Tags::Point{}, vertex_count);

				break;
			} break;
			case GeometryType::LINESTRING: {
				const auto vertex_count = reader.Read<uint32_t, IS_BIG_ENDIAN>();
				auto reader_copy = reader;
				reader.Skip(vertex_count * sizeof(VERTEX_TYPE));

				static_cast<IMPL*>(this)->template Enter<VERTEX_TYPE>(Tags::Line{}, vertex_count);
				static_cast<IMPL*>(this)->template Vertices<VERTEX_TYPE, IS_BIG_ENDIAN>(reader_copy, vertex_count);
				static_cast<IMPL*>(this)->template Leave<VERTEX_TYPE>(Tags::Line{}, vertex_count);

			} break;
			case GeometryType::POLYGON: {
				const auto ring_count = reader.Read<uint32_t, IS_BIG_ENDIAN>();
				static_cast<IMPL*>(this)->template Enter<VERTEX_TYPE>(Tags::Polygon{}, ring_count);

				for (uint32_t i = 0; i < ring_count; i++) {
					const auto vertex_count = reader.Read<uint32_t, IS_BIG_ENDIAN>();
					static_cast<IMPL*>(this)->template Enter<VERTEX_TYPE>(Tags::Ring{}, vertex_count);
					auto reader_copy = reader;
					reader.Skip(vertex_count * sizeof(VERTEX_TYPE));
					static_cast<IMPL*>(this)->template Vertices<VERTEX_TYPE, IS_BIG_ENDIAN>(reader_copy, vertex_count);

					static_cast<IMPL*>(this)->template Leave<VERTEX_TYPE>(Tags::Ring{}, vertex_count);
				}

				static_cast<IMPL*>(this)->template Leave<VERTEX_TYPE>(Tags::Polygon{}, ring_count);
			} break;
			case GeometryType::MULTIPOINT: {
				auto count = reader.Read<uint32_t, IS_BIG_ENDIAN>();
				static_cast<IMPL*>(this)->template Enter<VERTEX_TYPE>(Tags::MultiPoint{}, count);
				for (uint32_t i = 0; i < count; i++) {
					Visit(reader);
				}
				static_cast<IMPL*>(this)->template Leave<VERTEX_TYPE>(Tags::MultiPoint{}, count);
			} break;
			case GeometryType::MULTILINESTRING: {
				auto count = reader.Read<uint32_t, IS_BIG_ENDIAN>();
				static_cast<IMPL*>(this)->template Enter<VERTEX_TYPE>(Tags::MultiLine{}, count);
				for (uint32_t i = 0; i < count; i++) {
					Visit(reader);
				}
				static_cast<IMPL*>(this)->template Leave<VERTEX_TYPE>(Tags::MultiLine{}, count);
			} break;
			case GeometryType::MULTIPOLYGON: {
				auto count = reader.Read<uint32_t, IS_BIG_ENDIAN>();
				static_cast<IMPL*>(this)->template Enter<VERTEX_TYPE>(Tags::MultiPolygon{}, count);
				for (uint32_t i = 0; i < count; i++) {
					Visit(reader);
				}
				static_cast<IMPL*>(this)->template Leave<VERTEX_TYPE>(Tags::MultiPolygon{}, count);
			} break;
			case GeometryType::GEOMETRYCOLLECTION: {
				auto count = reader.Read<uint32_t, IS_BIG_ENDIAN>();
				static_cast<IMPL*>(this)->template Enter<VERTEX_TYPE>(Tags::GeometryCollection{}, count);
				for (uint32_t i = 0; i < count; i++) {
					Visit(reader);
				}
				static_cast<IMPL*>(this)->template Leave<VERTEX_TYPE>(Tags::GeometryCollection{}, count);
			} break;
			default:
				throw InvalidInputException("GeometryVisitor: Unsupported geometry type %d", static_cast<int>(type));
		}
	}

	template<bool IS_BIG_ENDIAN>
	void VisitWKB(BinaryReader &reader) {
		const auto header = reader.Read<uint32_t, IS_BIG_ENDIAN>();
		const auto flag = (header & 0xFFFF) / 1000;
		const auto type = (header & 0xFFFF) % 1000;

		const auto has_z = (flag & 0x01) != 0;
		const auto has_m = (flag & 0x02) != 0;

		if (has_z && has_m) {
			VisitWKBInternal<VertexXYZM, IS_BIG_ENDIAN>(reader, static_cast<GeometryType>(type));
		} else if (has_z) {
			VisitWKBInternal<VertexXYZ, IS_BIG_ENDIAN>(reader, static_cast<GeometryType>(type));
		} else if (has_m) {
			VisitWKBInternal<VertexXYM, IS_BIG_ENDIAN>(reader, static_cast<GeometryType>(type));
		} else {
			VisitWKBInternal<VertexXY, IS_BIG_ENDIAN>(reader, static_cast<GeometryType>(type));
		}
	}


	template<class VERTEX_TYPE>
	void VisitBKBInternal(BinaryReader &reader, const BKBMeta &meta) {

		const auto count = meta.GetCount();

		switch (meta.GetType()) {
			case GeometryType::POINT: {
				auto reader_copy = reader;
				static_cast<IMPL*>(this)->template Enter<VERTEX_TYPE>(Tags::Point{}, count);
				static_cast<IMPL*>(this)->template Vertices<VERTEX_TYPE, false>(reader_copy, count);
				static_cast<IMPL*>(this)->template Leave<VERTEX_TYPE>(Tags::Point{}, count);
				reader.Skip(count * sizeof(VERTEX_TYPE));
			} break;
			case GeometryType::LINESTRING: {
				auto reader_copy = reader;
				static_cast<IMPL*>(this)->template Enter<VERTEX_TYPE>(Tags::Line{}, count);
				static_cast<IMPL*>(this)->template Vertices<VERTEX_TYPE, false>(reader_copy, count);
				static_cast<IMPL*>(this)->template Leave<VERTEX_TYPE>(Tags::Line{}, count);
				reader.Skip(count * sizeof(VERTEX_TYPE));
			} break;
			case GeometryType::POLYGON: {
				static_cast<IMPL*>(this)->template Enter<VERTEX_TYPE>(Tags::Polygon{}, count);
				for (uint32_t i = 0; i < count; i++) {
					auto ring_meta = reader.Read<BKBMeta>();
					ring_meta.Verify();

					auto reader_copy = reader;
					static_cast<IMPL*>(this)->template Enter<VERTEX_TYPE>(Tags::Ring{}, ring_meta.GetCount());
					static_cast<IMPL*>(this)->template Vertices<VERTEX_TYPE, false>(reader_copy, ring_meta.GetCount());
					static_cast<IMPL*>(this)->template Leave<VERTEX_TYPE>(Tags::Ring{}, ring_meta.GetCount());
					reader.Skip(ring_meta.GetCount() * sizeof(VERTEX_TYPE));
				}
				static_cast<IMPL*>(this)->template Leave<VERTEX_TYPE>(Tags::Polygon{}, count);
			} break;
			case GeometryType::MULTIPOINT: {
				static_cast<IMPL*>(this)->template Enter<VERTEX_TYPE>(Tags::MultiPoint{}, count);
				for (uint32_t i = 0; i < count; i++) {
					VisitBKB(reader);
				}
				static_cast<IMPL*>(this)->template Leave<VERTEX_TYPE>(Tags::MultiPoint{}, count);
			} break;
			case GeometryType::MULTILINESTRING: {
				static_cast<IMPL*>(this)->template Enter<VERTEX_TYPE>(Tags::MultiLine{}, count);
				for (uint32_t i = 0; i < count; i++) {
					VisitBKB(reader);
				}
				static_cast<IMPL*>(this)->template Leave<VERTEX_TYPE>(Tags::MultiLine{}, count);
			} break;
			case GeometryType::MULTIPOLYGON: {
				static_cast<IMPL*>(this)->template Enter<VERTEX_TYPE>(Tags::MultiPolygon{}, count);
				for (uint32_t i = 0; i < count; i++) {
					VisitBKB(reader);
				}
				static_cast<IMPL*>(this)->template Leave<VERTEX_TYPE>(Tags::MultiPolygon{}, count);
			} break;
			case GeometryType::GEOMETRYCOLLECTION: {
				static_cast<IMPL*>(this)->template Enter<VERTEX_TYPE>(Tags::GeometryCollection{}, count);
				for (uint32_t i = 0; i < count; i++) {
					VisitBKB(reader);
				}
				static_cast<IMPL*>(this)->template Leave<VERTEX_TYPE>(Tags::GeometryCollection{}, count);
			} break;
			default:
				throw InvalidInputException("GeometryVisitor: Unsupported geometry type %d", static_cast<int>(meta.GetType()));
		}
	}

	void VisitBKB(BinaryReader &reader) {
		const auto meta = reader.Read<BKBMeta>();
		meta.Verify();

		if (meta.HasZ() && meta.HasM()) {
			VisitBKBInternal<VertexXYZM>(reader, meta);
		} else if (meta.HasZ()) {
			VisitBKBInternal<VertexXYZ>(reader, meta);
		} else if (meta.HasM()) {
			VisitBKBInternal<VertexXYM>(reader, meta);
		} else {
			VisitBKBInternal<VertexXY>(reader, meta);
		}
	}

	void Visit(BinaryReader &reader) {
		const auto be = reader.Read<uint8_t>();
		switch (be) {
		case 0x00: // Big-endian
			VisitWKB<true>(reader);
			break;
		case 0x01: // Little-endian
			VisitWKB<false>(reader);
			break;
		case 0x02: // BKB (Better Known Binary)
			reader.Rollback(sizeof(uint8_t));
			VisitBKB(reader);
			break;
		default:
			throw InvalidInputException("GeometryVisitor: Unsupported byte order %02x", be);
		}
	}
public:
	void Visit(const char* data, size_t size) {
		BinaryReader reader(data, size);
		Visit(reader);
	}
};


} // namespace
} // namespace duckdb
//======================================================================================================================
// Scalar Functions
//======================================================================================================================
namespace duckdb {
namespace {

struct WKB_FromBlob {

	struct ToLittleEndianWKB : public GeometryVisitor<ToLittleEndianWKB> {
		BinaryWriter writer;
		bool in_point = false;

		template<class VERTEX_TYPE = VertexXY, bool IS_BIG_ENDIAN = false>
		void Vertices(BinaryReader &reader, uint32_t vertex_count) {
			if (in_point) {
				if (vertex_count == 0) {
					constexpr auto nan = std::numeric_limits<double>::quiet_NaN();
					constexpr double nan_array[] = {nan, nan, nan, nan};
					writer.Copy(reinterpret_cast<const char*>(&nan_array), sizeof(VERTEX_TYPE));
				} else {
					auto vertex = reader.Read<VERTEX_TYPE, IS_BIG_ENDIAN>();
					writer.Write(vertex);
				}
				return;
			}

			writer.Write<uint32_t>(vertex_count);
			writer.Reserve(vertex_count * sizeof(VERTEX_TYPE));
			for (uint32_t i = 0; i < vertex_count; i++) {
				auto vertex = reader.Read<VERTEX_TYPE, IS_BIG_ENDIAN>();
				writer.Write(vertex);
			}
		}

		template<class VERTEX_TYPE>
		void Enter(Tags::Point, uint32_t count) {
			in_point = true;
			writer.Write<uint8_t>(1);
			auto flag = static_cast<uint32_t>(GeometryType::POINT);
			if (VERTEX_TYPE::HAS_Z) { flag += 1000; }
			if (VERTEX_TYPE::HAS_M) { flag += 2000; }
			writer.Write<uint32_t>(flag);
		}

		template<class VERTEX_TYPE>
		void Leave(Tags::Point) {
			in_point = false;
		}

		template<class VERTEX_TYPE>
		void Enter(Tags::Line, uint32_t count) {
			writer.Write<uint8_t>(1);
			auto flag = static_cast<uint32_t>(GeometryType::LINESTRING);
			if (VERTEX_TYPE::HAS_Z) { flag += 1000; }
			if (VERTEX_TYPE::HAS_M) { flag += 2000; }
			writer.Write<uint32_t>(flag);
		}
		template<class VERTEX_TYPE>
		void Enter(Tags::Polygon, uint32_t count) {
			writer.Write<uint8_t>(1);
			auto flag = static_cast<uint32_t>(GeometryType::POLYGON);
			if (VERTEX_TYPE::HAS_Z) { flag += 1000; }
			if (VERTEX_TYPE::HAS_M) { flag += 2000; }
			writer.Write<uint32_t>(flag);

			writer.Write<uint32_t>(count); // Write the number of rings
		}
		template<class VERTEX_TYPE>
		void Enter(Tags::Ring, uint32_t count) { /* Do nothing */ }

		template<class VERTEX_TYPE>
		void Enter(Tags::MultiPoint, uint32_t count) {
			writer.Write<uint8_t>(1);
			auto flag = static_cast<uint32_t>(GeometryType::MULTIPOINT);
			if (VERTEX_TYPE::HAS_Z) { flag += 1000; }
			if (VERTEX_TYPE::HAS_M) { flag += 2000; }
			writer.Write<uint32_t>(flag);

			writer.Write<uint32_t>(count); // Write the number of points
		}

		template<class VERTEX_TYPE>
		void Enter(Tags::MultiLine, uint32_t count) {
			writer.Write<uint8_t>(1);
			auto flag = static_cast<uint32_t>(GeometryType::MULTILINESTRING);
			if (VERTEX_TYPE::HAS_Z) { flag += 1000; }
			if (VERTEX_TYPE::HAS_M) { flag += 2000; }
			writer.Write<uint32_t>(flag);

			writer.Write<uint32_t>(count); // Write the number of lines
		}

		template<class VERTEX_TYPE>
		void Enter(Tags::MultiPolygon, uint32_t count) {
			writer.Write<uint8_t>(1);
			auto flag = static_cast<uint32_t>(GeometryType::MULTIPOLYGON);
			if (VERTEX_TYPE::HAS_Z) { flag += 1000; }
			if (VERTEX_TYPE::HAS_M) { flag += 2000; }
			writer.Write<uint32_t>(flag);

			writer.Write<uint32_t>(count); // Write the number of polygons
		}

		template<class VERTEX_TYPE>
		void Enter(Tags::GeometryCollection, uint32_t count) {
			writer.Write<uint8_t>(1);
			auto flag = static_cast<uint32_t>(GeometryType::GEOMETRYCOLLECTION);
			if (VERTEX_TYPE::HAS_Z) { flag += 1000; }
			if (VERTEX_TYPE::HAS_M) { flag += 2000; }
			writer.Write<uint32_t>(flag);

			writer.Write<uint32_t>(count); // Write the number of geometries in the collection
		}

		template<class VERTEX_TYPE>
		void Leave(Tags::Any, uint32_t count) { /* Do nothing */ }
	};

	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {

		ToLittleEndianWKB visitor;
		UnaryExecutor::Execute<string_t, string_t>(
		    args.data[0], result, args.size(),
		    [&](const string_t &input) {
		    	visitor.writer.Reset();
		    	visitor.writer.Reserve(input.GetSize()); // Should be the same size as the input

			    visitor.Visit(input.GetData(), input.GetSize());

		    	return StringVector::AddStringOrBlob(result, visitor.writer.GetData(), visitor.writer.GetSize());
		    });

	}

	static void ExecuteReinterpret(DataChunk &args, ExpressionState &state, Vector &result) {
		result.Reinterpret(args.data[0]);
	}

	static void Register(DatabaseInstance &db) {
		ScalarFunction reinterpret_func(
		    "wkb_reinterpret_blob", {LogicalType::BLOB}, Types::WKB(), ExecuteReinterpret);
		ExtensionUtil::RegisterFunction(db, std::move(reinterpret_func));

		ScalarFunction func("wkb_from_blob", {LogicalType::BLOB}, Types::WKB(), Execute);
		ExtensionUtil::RegisterFunction(db, std::move(func));
	}
};

struct BKB_FromBlob {

	struct ToBKBVisitor : public GeometryVisitor<ToBKBVisitor> {
		BinaryWriter writer;

		template<class VERTEX_TYPE = VertexXY, bool IS_BIG_ENDIAN>
		void Vertices(BinaryReader &reader, uint32_t vertex_count) {
			// Copy vertices straight up
			if (!IS_BIG_ENDIAN) {
				const auto size = vertex_count * sizeof(VERTEX_TYPE);
				const auto data = reader.Reserve(size);
				writer.CopyTemplated<VERTEX_TYPE>(data, vertex_count);
			}
			else {
				for (uint32_t i = 0; i < vertex_count; i++) {
					auto vertex = reader.Read<VERTEX_TYPE, IS_BIG_ENDIAN>();
					writer.Write(vertex);
				}
			}
		}

		void WriteMeta(GeometryType type, bool has_z, bool has_m, uint32_t count) {
			BKBMeta meta;
			meta.meta[0] = 0x02; // BKB header
			meta.meta[1] = 0x01; // Version
			meta.meta[2] = (has_z ? 0x01 : 0x00) | (has_m ? 0x02 : 0x00); // Z and M flags
			meta.meta[3] = static_cast<uint8_t>(type); // Geometry type
			meta.size = count; // Vertex count
			writer.Write<BKBMeta>(meta);
		}
		template<class VERTEX_TYPE>
		void Enter(Tags::Point, uint32_t count) {
			WriteMeta(GeometryType::POINT, VERTEX_TYPE::HAS_Z, VERTEX_TYPE::HAS_M, count);
		}
		template<class VERTEX_TYPE>
		void Enter(Tags::Line, uint32_t count) {
			WriteMeta(GeometryType::LINESTRING, VERTEX_TYPE::HAS_Z, VERTEX_TYPE::HAS_M, count);
		}
		template<class VERTEX_TYPE>
		void Enter(Tags::Polygon, uint32_t count) {
			WriteMeta(GeometryType::POLYGON, VERTEX_TYPE::HAS_Z, VERTEX_TYPE::HAS_M, count);
		}
		template<class VERTEX_TYPE>
		void Enter(Tags::Ring, uint32_t count) {
			// Write rings as linestrings
			WriteMeta(GeometryType::LINESTRING, VERTEX_TYPE::HAS_Z, VERTEX_TYPE::HAS_M, count);
		}
		template<class VERTEX_TYPE>
		void Enter(Tags::MultiPoint, uint32_t count) {
			WriteMeta(GeometryType::MULTIPOINT, VERTEX_TYPE::HAS_Z, VERTEX_TYPE::HAS_M, count);
		}
		template<class VERTEX_TYPE>
		void Enter(Tags::MultiLine, uint32_t count) {
			WriteMeta(GeometryType::MULTILINESTRING, VERTEX_TYPE::HAS_Z, VERTEX_TYPE::HAS_M, count);
		}
		template<class VERTEX_TYPE>
		void Enter(Tags::MultiPolygon, uint32_t count) {
			WriteMeta(GeometryType::MULTIPOLYGON, VERTEX_TYPE::HAS_Z, VERTEX_TYPE::HAS_M, count);
		}
		template<class VERTEX_TYPE>
		void Enter(Tags::GeometryCollection, uint32_t count) {
			WriteMeta(GeometryType::GEOMETRYCOLLECTION, VERTEX_TYPE::HAS_Z, VERTEX_TYPE::HAS_M, count);
		}

		// Leave methods do nothing for BKB
		template<class VERTEX_TYPE>
		void Leave(Tags::Any, uint32_t) {}
	};

	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
		ToBKBVisitor visitor;
		UnaryExecutor::Execute<string_t, string_t>(
			args.data[0], result, args.size(),
			[&](const string_t &input) {
				visitor.writer.Reset();
				visitor.Visit(input.GetData(), input.GetSize());
				return StringVector::AddStringOrBlob(result, visitor.writer.GetData(), visitor.writer.GetSize());
			});
	}

	static void Register(DatabaseInstance &db) {
		ScalarFunction func("bkb_from_blob", {LogicalType::BLOB}, Types::BKB(), Execute);
		ExtensionUtil::RegisterFunction(db, std::move(func));
	}
};

struct ST_Extent {

	struct ExtentVisitor : public GeometryVisitor<ExtentVisitor> {

		double min_x = std::numeric_limits<double>::max();
		double min_y = std::numeric_limits<double>::max();
		double max_x = std::numeric_limits<double>::lowest();
		double max_y = std::numeric_limits<double>::lowest();
		uint32_t total_vertices = 0;

		template<class VERTEX_TYPE = VertexXY, bool IS_BIG_ENDIAN = false>
		void Vertices(BinaryReader &reader, uint32_t vertex_count) {
			total_vertices += vertex_count;

			if (reader.IsAligned(alignof(VERTEX_TYPE))) { // This line is absolutely critical, gives about 30% performance boost!!!!
				auto ptr = reinterpret_cast<const VERTEX_TYPE*>(reader.Reserve(vertex_count * sizeof(VERTEX_TYPE)));
				for (uint32_t i = 0; i < vertex_count; i++) {
					// Just dereference the pointer directly
					auto vertex = ptr[i];
					min_x = std::min(min_x, vertex.x);
					min_y = std::min(min_y, vertex.y);
					max_x = std::max(max_x, vertex.x);
					max_y = std::max(max_y, vertex.y);
				}
			} else {
				for (uint32_t i = 0; i < vertex_count; i++) {
					// Make a memcpy (in .Read)
					auto vertex = reader.Read<VERTEX_TYPE, IS_BIG_ENDIAN>();
					min_x = std::min(min_x, vertex.x);
					min_y = std::min(min_y, vertex.y);
					max_x = std::max(max_x, vertex.x);
					max_y = std::max(max_y, vertex.y);
				}
			}
		}

		template<class V> void Enter(Tags::Any, uint32_t) {} // Do nothing for generic entry
		template<class V> void Leave(Tags::Any, uint32_t) {} // Do nothing for generic leave
	};

	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
		const auto &bbox_vec = StructVector::GetEntries(result);
		const auto min_x_data = FlatVector::GetData<double>(*bbox_vec[0]);
		const auto min_y_data = FlatVector::GetData<double>(*bbox_vec[1]);
		const auto max_x_data = FlatVector::GetData<double>(*bbox_vec[2]);
		const auto max_y_data = FlatVector::GetData<double>(*bbox_vec[3]);

		UnifiedVectorFormat input_vdata;
		args.data[0].ToUnifiedFormat(args.size(), input_vdata);
		const auto input_data = UnifiedVectorFormat::GetData<string_t>(input_vdata);

		const auto count = args.size();

		for (idx_t out_idx = 0; out_idx < count; out_idx++) {
			const auto row_idx = input_vdata.sel->get_index(out_idx);
			if (!input_vdata.validity.RowIsValid(row_idx)) {
				// null in -> null out
				FlatVector::SetNull(result, out_idx, true);
				continue;
			}

			const auto &blob = input_data[row_idx];

			ExtentVisitor visitor;
			visitor.Visit(blob.GetData(), blob.GetSize());
			if (visitor.total_vertices == 0) {
				FlatVector::SetNull(result, out_idx, true);
				continue;
			}

			min_x_data[out_idx] = visitor.min_x;
			min_y_data[out_idx] = visitor.min_y;
			max_x_data[out_idx] = visitor.max_x;
			max_y_data[out_idx] = visitor.max_y;
		}

		if (args.AllConstant()) {
			result.SetVectorType(VectorType::CONSTANT_VECTOR);
		}
	}

	static void ExecuteBKBNonRecursive(DataChunk &args, ExpressionState &state, Vector &result) {
		const auto &bbox_vec = StructVector::GetEntries(result);
		const auto min_x_data = FlatVector::GetData<double>(*bbox_vec[0]);
		const auto min_y_data = FlatVector::GetData<double>(*bbox_vec[1]);
		const auto max_x_data = FlatVector::GetData<double>(*bbox_vec[2]);
		const auto max_y_data = FlatVector::GetData<double>(*bbox_vec[3]);

		UnifiedVectorFormat input_vdata;
		args.data[0].ToUnifiedFormat(args.size(), input_vdata);
		const auto input_data = UnifiedVectorFormat::GetData<string_t>(input_vdata);

		const auto count = args.size();

		for (idx_t out_idx = 0; out_idx < count; out_idx++) {
			const auto row_idx = input_vdata.sel->get_index(out_idx);
			if (!input_vdata.validity.RowIsValid(row_idx)) {
				// null in -> null out
				FlatVector::SetNull(result, out_idx, true);
				continue;
			}

			const auto &blob = input_data[row_idx];
			const auto blob_ptr = blob.GetData();
			const auto blob_len = blob.GetSize();

			auto min_x = std::numeric_limits<double>::max();
			auto min_y = std::numeric_limits<double>::max();
			auto max_x = std::numeric_limits<double>::lowest();
			auto max_y = std::numeric_limits<double>::lowest();
			uint32_t total_vertices = 0;

			BinaryReader reader(blob_ptr, blob_len);
			while (!reader.IsAtEnd()) {
				const auto meta = reader.Read<BKBMeta>();
				meta.Verify();
				switch (meta.GetType()) {
					case GeometryType::POINT:
					case GeometryType::LINESTRING: {
						const auto vertex_parts = static_cast<VertexType>(meta.HasZ() + (meta.HasM() * 2));
						const auto vertex_count = meta.GetCount();

						total_vertices += vertex_count;

						switch (vertex_parts) {
						case VertexType::XY: {
							for (uint32_t i = 0; i < vertex_count; i++) {
								const auto v = reader.Read<VertexXY>();
								min_x = std::min(min_x, v.x);
								min_y = std::min(min_y, v.y);
								max_x = std::max(max_x, v.x);
								max_y = std::max(max_y, v.y);
							}
						} break;
						case VertexType::XYM: {
							for (uint32_t i = 0; i < vertex_count; i++) {
								const auto v = reader.Read<VertexXYM>();
								min_x = std::min(min_x, v.x);
								min_y = std::min(min_y, v.y);
								max_x = std::max(max_x, v.x);
								max_y = std::max(max_y, v.y);
							}
						} break;
						case VertexType::XYZ: {
							for (uint32_t i = 0; i < vertex_count; i++) {
								const auto v = reader.Read<VertexXYZ>();
								min_x = std::min(min_x, v.x);
								min_y = std::min(min_y, v.y);
								max_x = std::max(max_x, v.x);
								max_y = std::max(max_y, v.y);
							}
						} break;
						case VertexType::XYZM: {
							for (uint32_t i = 0; i < vertex_count; i++) {
								const auto v = reader.Read<VertexXYZM>();
								min_x = std::min(min_x, v.x);
								min_y = std::min(min_y, v.y);
								max_x = std::max(max_x, v.x);
								max_y = std::max(max_y, v.y);
							}
						} break;
						default:
							throw InvalidInputException("Unknown vertex type");
						}
					} break;
					case GeometryType::POLYGON:
					case GeometryType::MULTIPOINT:
					case GeometryType::MULTILINESTRING:
					case GeometryType::MULTIPOLYGON:
					case GeometryType::GEOMETRYCOLLECTION:
						continue;
					default:
						throw InvalidInputException("Unknown meta type");
				}
			}

			if (total_vertices == 0) {
				FlatVector::SetNull(result, out_idx, true);
				continue;
			}


			min_x_data[out_idx] = min_x;
			min_y_data[out_idx] = min_y;
			max_x_data[out_idx] = max_x;
			max_y_data[out_idx] = max_y;
		}

		if (args.AllConstant()) {
			result.SetVectorType(VectorType::CONSTANT_VECTOR);
		}
	}

	static void ExecuteWKBNonRecursive(DataChunk &args, ExpressionState &state, Vector &result) {
		const auto &bbox_vec = StructVector::GetEntries(result);
		const auto min_x_data = FlatVector::GetData<double>(*bbox_vec[0]);
		const auto min_y_data = FlatVector::GetData<double>(*bbox_vec[1]);
		const auto max_x_data = FlatVector::GetData<double>(*bbox_vec[2]);
		const auto max_y_data = FlatVector::GetData<double>(*bbox_vec[3]);

		UnifiedVectorFormat input_vdata;
		args.data[0].ToUnifiedFormat(args.size(), input_vdata);
		const auto input_data = UnifiedVectorFormat::GetData<string_t>(input_vdata);

		const auto count = args.size();

		for (idx_t out_idx = 0; out_idx < count; out_idx++) {
			const auto row_idx = input_vdata.sel->get_index(out_idx);
			if (!input_vdata.validity.RowIsValid(row_idx)) {
				// null in -> null out
				FlatVector::SetNull(result, out_idx, true);
				continue;
			}

			const auto &blob = input_data[row_idx];
			const auto blob_ptr = blob.GetData();
			const auto blob_len = blob.GetSize();

			auto min_x = std::numeric_limits<double>::max();
			auto min_y = std::numeric_limits<double>::max();
			auto max_x = std::numeric_limits<double>::lowest();
			auto max_y = std::numeric_limits<double>::lowest();
			uint32_t total_vertices = 0;

			BinaryReader reader(blob_ptr, blob_len);
			while (!reader.IsAtEnd()) {

				const auto meta = reader.Read<WKBMeta>();
				if (meta.LE() != 0x01) {
					throw InvalidInputException("GeometryVisitor: Expected little-endian byte order, got %02x", meta.LE());
				}
				const auto type_id = meta.GetType();
				const auto type = static_cast<GeometryType>(type_id % 1000);
				const auto flag = type_id / 1000;
				const auto has_z = (flag & 0x01) != 0;
				const auto has_m = (flag & 0x02) != 0;

				switch (type) {
					case GeometryType::POINT: {
						const auto x = reader.Read<double>();
						const auto y = reader.Read<double>();

						if (std::isnan(x) && std::isnan(y)) {
							// Skip NaN points
							continue;
						}

						min_x = std::min(min_x, x);
						min_y = std::min(min_y, y);
						max_x = std::max(max_x, x);
						max_y = std::max(max_y, y);

						total_vertices += 1;

						if (has_z) {
							reader.Read<double>(); // Skip Z
						}
						if (has_m) {
							reader.Read<double>(); // Skip M
						}
					} break;
					case GeometryType::LINESTRING: {
						const auto vertex_parts = static_cast<VertexType>(has_z + has_m);
						const auto vertex_count = reader.Read<uint32_t>();

						total_vertices += vertex_count;

						switch (vertex_parts) {
						case VertexType::XY: {
							for (uint32_t i = 0; i < vertex_count; i++) {
								const auto v = reader.Read<VertexXY>();
								min_x = std::min(min_x, v.x);
								min_y = std::min(min_y, v.y);
								max_x = std::max(max_x, v.x);
								max_y = std::max(max_y, v.y);
							}
						} break;
						case VertexType::XYM: {
							for (uint32_t i = 0; i < vertex_count; i++) {
								const auto v = reader.Read<VertexXYM>();
								min_x = std::min(min_x, v.x);
								min_y = std::min(min_y, v.y);
								max_x = std::max(max_x, v.x);
								max_y = std::max(max_y, v.y);
							}
						} break;
						case VertexType::XYZ: {
							for (uint32_t i = 0; i < vertex_count; i++) {
								const auto v = reader.Read<VertexXYZ>();
								min_x = std::min(min_x, v.x);
								min_y = std::min(min_y, v.y);
								max_x = std::max(max_x, v.x);
								max_y = std::max(max_y, v.y);
							}
						} break;
						case VertexType::XYZM: {
							for (uint32_t i = 0; i < vertex_count; i++) {
								const auto v = reader.Read<VertexXYZM>();
								min_x = std::min(min_x, v.x);
								min_y = std::min(min_y, v.y);
								max_x = std::max(max_x, v.x);
								max_y = std::max(max_y, v.y);
							}
						} break;
						default:
							throw InvalidInputException("Unknown vertex type");
						}
					} break;
					case GeometryType::POLYGON: {
						const auto vertex_parts = static_cast<VertexType>(has_z + has_m);
						const auto ring_count = reader.Read<uint32_t>();

						for (uint32_t r = 0; r < ring_count; r++) {
							const auto vertex_count = reader.Read<uint32_t>();
							total_vertices += vertex_count;

							switch (vertex_parts) {
							case VertexType::XY: {
								for (uint32_t i = 0; i < vertex_count; i++) {
									const auto v = reader.Read<VertexXY>();
									min_x = std::min(min_x, v.x);
									min_y = std::min(min_y, v.y);
									max_x = std::max(max_x, v.x);
									max_y = std::max(max_y, v.y);
								}
							} break;
							case VertexType::XYM: {
								for (uint32_t i = 0; i < vertex_count; i++) {
									const auto v = reader.Read<VertexXYM>();
									min_x = std::min(min_x, v.x);
									min_y = std::min(min_y, v.y);
									max_x = std::max(max_x, v.x);
									max_y = std::max(max_y, v.y);
								}
							} break;
							case VertexType::XYZ: {
								for (uint32_t i = 0; i < vertex_count; i++) {
									const auto v = reader.Read<VertexXYZ>();
									min_x = std::min(min_x, v.x);
									min_y = std::min(min_y, v.y);
									max_x = std::max(max_x, v.x);
									max_y = std::max(max_y, v.y);
								}
							} break;
							case VertexType::XYZM: {
								for (uint32_t i = 0; i < vertex_count; i++) {
									const auto v = reader.Read<VertexXYZM>();
									min_x = std::min(min_x, v.x);
									min_y = std::min(min_y, v.y);
									max_x = std::max(max_x, v.x);
									max_y = std::max(max_y, v.y);
								}
							} break;
							default:
								throw InvalidInputException("Unknown vertex type");
							}
						}
					} break;
					case GeometryType::MULTIPOINT:
					case GeometryType::MULTILINESTRING:
					case GeometryType::MULTIPOLYGON:
					case GeometryType::GEOMETRYCOLLECTION:
						reader.Read<uint32_t>(); // Read the count, but ignore the geometries
						continue;;
					default:
						throw InvalidInputException("Unknown meta type %d", static_cast<int>(type));
				}
			}


			if (total_vertices == 0) {
				FlatVector::SetNull(result, out_idx, true);
				continue;
			}

			min_x_data[out_idx] = min_x;
			min_y_data[out_idx] = min_y;
			max_x_data[out_idx] = max_x;
			max_y_data[out_idx] = max_y;
		}

		if (args.AllConstant()) {
			result.SetVectorType(VectorType::CONSTANT_VECTOR);
		}
	}

	static void Register(DatabaseInstance &db) {
		const auto box_type = LogicalType::STRUCT({{"min_x", LogicalType::DOUBLE},
																{"min_y", LogicalType::DOUBLE},
																{"max_x", LogicalType::DOUBLE},
																{"max_y", LogicalType::DOUBLE}});
		ScalarFunctionSet set("st_extent");
		set.AddFunction(ScalarFunction({Types::WKB()}, box_type, Execute));
		set.AddFunction(ScalarFunction({Types::BKB()}, box_type, Execute));
		ExtensionUtil::RegisterFunction(db, std::move(set));

		ScalarFunctionSet fast_set("st_extent_non_recursive");
		fast_set.AddFunction(ScalarFunction({Types::WKB()}, box_type, ExecuteWKBNonRecursive));
		fast_set.AddFunction(ScalarFunction({Types::BKB()}, box_type, ExecuteBKBNonRecursive));
		ExtensionUtil::RegisterFunction(db, std::move(fast_set));
	}
};

struct ST_AsWKB {
	static uint32_t GetRequiredSize(BinaryReader &reader) {
		uint32_t total_size = 0;
		while (!reader.IsAtEnd()) {
			const auto meta = reader.Read<BKBMeta>();
			meta.Verify();

			total_size += 1 + 4; // 1 byte for byte order, 4 bytes for type

			switch (meta.GetType()) {
				case GeometryType::POINT: {
					const auto vertex_width = (2 + meta.HasZ() + meta.HasM()) * sizeof(double);
					const auto vertex_count = meta.GetCount();
					total_size += vertex_width;
					reader.Skip(vertex_width * vertex_count); // Skip the vertex data
				} break;
				case GeometryType::LINESTRING: {
					const auto vertex_width = (2 + meta.HasZ() + meta.HasM()) * sizeof(double);
					const auto vertex_count = meta.GetCount();
					total_size += 4 + vertex_count * vertex_width; // 4 + vertex count + vertices
					reader.Skip(vertex_count * vertex_width); // Skip the vertex data
				} break;
				case GeometryType::POLYGON: {
					total_size += 4; // 4 bytes for ring count
					for (uint32_t ring_idx = 0; ring_idx < meta.GetCount(); ring_idx++) {
						const auto ring_meta = reader.Read<BKBMeta>();
						ring_meta.Verify();

						const auto vertex_width = (2 + ring_meta.HasZ() + ring_meta.HasM()) * sizeof(double);
						const auto vertex_count = ring_meta.GetCount();
						total_size += 4 + vertex_count * vertex_width; // 4 + vertex count + vertices

						reader.Skip(vertex_count * vertex_width);
					}
				} break;
				case GeometryType::MULTIPOINT:
				case GeometryType::MULTILINESTRING:
				case GeometryType::MULTIPOLYGON:
				case GeometryType::GEOMETRYCOLLECTION:
					total_size += 4; // 4 bytes for count
				break;
				default:
					throw InvalidInputException("Unknown meta type %d", static_cast<int>(meta.GetType()));
			}
		}
		return total_size;
	}

	static void Convert(BinaryReader &reader, FixedBinaryWriter<false> &writer) {
		while (!reader.IsAtEnd()) {
			const auto meta = reader.Read<BKBMeta>();
			meta.Verify();

			WKBMeta wkb_meta;
			wkb_meta.SetLE(1);
			wkb_meta.SetType(static_cast<uint32_t>(meta.GetType()) + (meta.HasZ() ? 1000 : 0) + (meta.HasM() ? 2000 : 0));
			writer.Write(wkb_meta);

			const auto count = meta.GetCount();
			const auto width = (2 + meta.HasZ() + meta.HasM()) * sizeof(double);

			switch (meta.GetType()) {
				case GeometryType::POINT: {
					if (count == 0) {
						constexpr auto nan = std::numeric_limits<double>::quiet_NaN();
						constexpr double nan_array[] = {nan, nan, nan, nan};
						writer.Copy(reinterpret_cast<const char*>(&nan_array), width);
					} else {
						writer.Copy(reader.Reserve(width), width);
					}
				} break;
				case GeometryType::LINESTRING: {
					writer.Write<uint32_t>(count);
					writer.Copy(reader.Reserve(width * count), width * count);
				} break;
				case GeometryType::POLYGON: {
					// Write the number of rings
					writer.Write<uint32_t>(count);
					for (uint32_t ring_idx = 0; ring_idx < count; ring_idx++) {
						auto ring_meta = reader.Read<BKBMeta>();
						ring_meta.Verify();
						const auto vertex_width = (2 + ring_meta.HasZ() + ring_meta.HasM()) * sizeof(double);
						const auto vertex_count = ring_meta.GetCount();
						writer.Write<uint32_t>(vertex_count);
						writer.Copy(reader.Reserve(vertex_width * vertex_count), vertex_width * vertex_count);
					}
				} break;
				case GeometryType::MULTIPOINT:
				case GeometryType::MULTILINESTRING:
				case GeometryType::MULTIPOLYGON:
				case GeometryType::GEOMETRYCOLLECTION: {
					// Write the number of geometries
					writer.Write<uint32_t>(count);
				} break;
				default:
					throw InvalidInputException("GeometryVisitor: Unknown meta type %d", static_cast<int>(meta.GetType()));

			}
		}
	}

	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
		UnaryExecutor::Execute<string_t, string_t>(
			args.data[0], result, args.size(),
			[&](const string_t &input) {

				BinaryReader reader(input.GetData(), input.GetSize());
				const auto size = GetRequiredSize(reader);
				auto blob = StringVector::EmptyString(result, size);
				FixedBinaryWriter<false> writer(blob.GetDataWriteable(), blob.GetSize());

				reader.Reset();
				Convert(reader, writer);
				blob.Finalize();

				return blob;
			});
	}

	static void ConvertDynamic(BinaryReader &reader, BinaryWriter &writer) {
		while (!reader.IsAtEnd()) {
			const auto meta = reader.Read<BKBMeta>();
			meta.Verify();

			writer.Write<uint8_t>(0x01); // Little-endian byte order
			writer.Write<uint32_t>(static_cast<uint32_t>(meta.GetType()) + (meta.HasZ() ? 1000 : 0) + (meta.HasM() ? 2000 : 0));

			const auto count = meta.GetCount();
			const auto width = (2 + meta.HasZ() + meta.HasM()) * sizeof(double);

			switch (meta.GetType()) {
			case GeometryType::POINT: {
				if (count == 0) {
					constexpr auto nan = std::numeric_limits<double>::quiet_NaN();
					constexpr double nan_array[] = {nan, nan, nan, nan};
					writer.Copy(reinterpret_cast<const char*>(&nan_array), width);
				} else {
					writer.Copy(reader.Reserve(width), width);
				}
			} break;
			case GeometryType::LINESTRING: {
				writer.Write<uint32_t>(count);
				writer.Copy(reader.Reserve(width * count), width * count);
			} break;
			case GeometryType::POLYGON: {
				// Write the number of rings
				writer.Write<uint32_t>(count);
				for (uint32_t ring_idx = 0; ring_idx < count; ring_idx++) {
					auto ring_meta = reader.Read<BKBMeta>();
					ring_meta.Verify();
					const auto vertex_width = (2 + ring_meta.HasZ() + ring_meta.HasM()) * sizeof(double);
					const auto vertex_count = ring_meta.GetCount();
					writer.Write<uint32_t>(vertex_count);
					writer.Copy(reader.Reserve(vertex_width * vertex_count), vertex_width * vertex_count);
				}
			} break;
			case GeometryType::MULTIPOINT:
			case GeometryType::MULTILINESTRING:
			case GeometryType::MULTIPOLYGON:
			case GeometryType::GEOMETRYCOLLECTION: {
				// Write the number of geometries
				writer.Write<uint32_t>(count);
			} break;
			default:
				throw InvalidInputException("GeometryVisitor: Unknown meta type %d", static_cast<int>(meta.GetType()));

			}
		}
	}


	static void ExecuteDynamic(DataChunk &args, ExpressionState &state, Vector &result) {
		BinaryWriter writer;
		UnaryExecutor::Execute<string_t, string_t>(
			args.data[0], result, args.size(),
			[&](const string_t &input) {

				writer.Reset();
				BinaryReader reader(input.GetData(), input.GetSize());
				ConvertDynamic(reader, writer);
				return StringVector::AddStringOrBlob(result, writer.GetData(), writer.GetSize());
			});
	}

	struct ToWKBVisitor : GeometryVisitor<ToWKBVisitor> {
		BinaryWriter writer;

		template<class VERTEX_TYPE = VertexXY, bool IS_BIG_ENDIAN>
		void Vertices(BinaryReader &reader, uint32_t vertex_count) {
			// Copy vertices straight up
			if (!IS_BIG_ENDIAN) {
				const auto size = vertex_count * sizeof(VERTEX_TYPE);
				const auto data = reader.Reserve(size);
				writer.CopyTemplated<VERTEX_TYPE>(data, vertex_count);
				return;
			}
			for (uint32_t i = 0; i < vertex_count; i++) {
				auto vertex = reader.Read<VERTEX_TYPE, IS_BIG_ENDIAN>();
				writer.Write(vertex);
			}
		}

		void WriteMeta(GeometryType type, bool has_z, bool has_m) {
			writer.Write<uint8_t>(0x01); // Little-endian byte order
			writer.Write<uint32_t>(static_cast<uint32_t>(type) + (has_z ? 1000 : 0) + (has_m ? 2000 : 0));
		}
		template<class VERTEX_TYPE>
		void Enter(Tags::Point, uint32_t count) {
			WriteMeta(GeometryType::POINT, VERTEX_TYPE::HAS_Z, VERTEX_TYPE::HAS_M);
			if (count == 0) {
				// write nan
				constexpr auto nan = std::numeric_limits<double>::quiet_NaN();
				constexpr double nan_array[] = {nan, nan, nan, nan};
				writer.Copy(reinterpret_cast<const char*>(&nan_array), sizeof(VERTEX_TYPE));
			}
		}
		template<class VERTEX_TYPE>
		void Enter(Tags::Line, uint32_t count) {
			WriteMeta(GeometryType::LINESTRING, VERTEX_TYPE::HAS_Z, VERTEX_TYPE::HAS_M);
			writer.Write<uint32_t>(count);
		}
		template<class VERTEX_TYPE>
		void Enter(Tags::Polygon, uint32_t count) {
			WriteMeta(GeometryType::POLYGON, VERTEX_TYPE::HAS_Z, VERTEX_TYPE::HAS_M);
			writer.Write<uint32_t>(count);
		}
		template<class VERTEX_TYPE>
		void Enter(Tags::Ring, uint32_t count) {
			// Write the ring count
			writer.Write<uint32_t>(count);
		}
		template<class VERTEX_TYPE>
		void Enter(Tags::MultiPoint, uint32_t count) {
			WriteMeta(GeometryType::MULTIPOINT, VERTEX_TYPE::HAS_Z, VERTEX_TYPE::HAS_M);
			writer.Write<uint32_t>(count);
		}
		template<class VERTEX_TYPE>
		void Enter(Tags::MultiLine, uint32_t count) {
			WriteMeta(GeometryType::MULTILINESTRING, VERTEX_TYPE::HAS_Z, VERTEX_TYPE::HAS_M);
			writer.Write<uint32_t>(count);
		}
		template<class VERTEX_TYPE>
		void Enter(Tags::MultiPolygon, uint32_t count) {
			WriteMeta(GeometryType::MULTIPOLYGON, VERTEX_TYPE::HAS_Z, VERTEX_TYPE::HAS_M);
			writer.Write<uint32_t>(count);
		}
		template<class VERTEX_TYPE>
		void Enter(Tags::GeometryCollection, uint32_t count) {
			WriteMeta(GeometryType::GEOMETRYCOLLECTION, VERTEX_TYPE::HAS_Z, VERTEX_TYPE::HAS_M);
			writer.Write<uint32_t>(count);
		}

		template<class VERTEX_TYPE>
		void Leave(Tags::Any, uint32_t) {}
	};

	static void ExecuteVisitor(DataChunk &args, ExpressionState &state, Vector &result) {
		ToWKBVisitor visitor;
		UnaryExecutor::Execute<string_t, string_t>(
			args.data[0], result, args.size(),
			[&](const string_t &input) {
				visitor.writer.Reset();
				visitor.Visit(input.GetData(), input.GetSize());
				return StringVector::AddStringOrBlob(result, visitor.writer.GetData(), visitor.writer.GetSize());
			});
	}

	static void Copy(DataChunk &args, ExpressionState &state, Vector &result) {
		UnaryExecutor::Execute<string_t, string_t>(
			args.data[0], result, args.size(),
			[&](const string_t &input) {
				return StringVector::AddStringOrBlob(result, input.GetData(), input.GetSize());
			});
	}

	static void Cast(DataChunk &args, ExpressionState &state, Vector &result) {
		result.Reinterpret(args.data[0]);
	}

	static void Register(DatabaseInstance &db) {
		ScalarFunction func("st_aswkb", {Types::BKB()}, LogicalType::BLOB, Execute);
		ExtensionUtil::RegisterFunction(db, std::move(func));

		ScalarFunction func1("st_aswkb_dynamic", {Types::BKB()}, LogicalType::BLOB, ExecuteDynamic);
		ExtensionUtil::RegisterFunction(db, std::move(func1));

		ScalarFunctionSet set_visitor("st_aswkb_visitor");
		set_visitor.AddFunction(ScalarFunction({Types::BKB()}, LogicalType::BLOB, ExecuteVisitor));
		set_visitor.AddFunction(ScalarFunction({Types::WKB()}, LogicalType::BLOB, ExecuteVisitor));
		ExtensionUtil::RegisterFunction(db, std::move(set_visitor));

		ScalarFunction func2("st_aswkb_copy", {Types::WKB()}, LogicalType::BLOB, Copy);
		ExtensionUtil::RegisterFunction(db, std::move(func2));
		ScalarFunction func3("st_aswkb_cast", {Types::WKB()}, LogicalType::BLOB, Cast);
		ExtensionUtil::RegisterFunction(db, std::move(func3));

		ScalarFunction func4("wkb_to_blob", {Types::WKB()}, LogicalType::BLOB, Cast);
		ExtensionUtil::RegisterFunction(db, std::move(func4));
	}
};

struct ST_Area {

	static void ExecuteWKB(DataChunk &args, ExpressionState &state, Vector &result) {
		UnaryExecutor::Execute<string_t, double>(args.data[0], result, args.size(),
			[&](const string_t &input) {

				double total_area = 0.0;
				BinaryReader reader(input.GetData(), input.GetSize());

				while (!reader.IsAtEnd()) {
					const auto le = reader.Read<uint8_t>();
					if (le != 0x01) {
						throw InvalidInputException("GeometryVisitor: Expected little-endian byte order, got %02x", le);
					}
					const auto type_id = reader.Read<uint32_t>();
					const auto type = static_cast<GeometryType>(type_id % 1000);
					const auto flag = type_id / 1000;
					const auto has_z = (flag & 0x01) != 0;
					const auto has_m = (flag & 0x02) != 0;

					const auto vertex_width = (2 + has_z + has_m) * sizeof(double);

					switch (type) {
						case GeometryType::POINT:
							reader.Skip(vertex_width); // Skip point
						break;
						case GeometryType::LINESTRING: {
							const auto vertex_count = reader.Read<uint32_t>();
							reader.Skip(vertex_width * vertex_count); // Skip vertices
						} break;
						case GeometryType::POLYGON: {
							const auto ring_count = reader.Read<uint32_t>();
							for (uint32_t i = 0; i < ring_count; ++i) {
								const auto vertex_count = reader.Read<uint32_t>();
								const auto vertex_array = reader.Reserve(vertex_count * vertex_width);

								double sum = 0.0;

								for (uint32_t vertex_idx = 0; vertex_idx < vertex_count - 1; vertex_idx++) {
									// Read vertices
									VertexXYZM v1;
									VertexXYZM v2;
									memcpy(&v1, vertex_array + vertex_idx * vertex_width, sizeof(VertexXY));
									memcpy(&v2, vertex_array + (vertex_idx + 1) * vertex_width, sizeof(VertexXY));

									sum += (v1.x * v2.y) - (v2.x * v1.y);
								}
								if (i == 0) {
									total_area += std::abs(sum) * 0.5; // First ring adds area
								} else {
									total_area -= std::abs(sum) * 0.5; // Subsequent rings subtract area
								}
							}
						} break;
						case GeometryType::MULTIPOINT:
						case GeometryType::MULTILINESTRING:
						case GeometryType::MULTIPOLYGON:
						case GeometryType::GEOMETRYCOLLECTION: {
							reader.Skip(sizeof(uint32_t)); // Skip count
						} break;
						default:
							throw InvalidInputException("GeometryVisitor: Unknown meta type %d", static_cast<int>(type));
					}
				}

				return total_area;
			});
	}

	static void Execute(DataChunk &args, ExpressionState &state, Vector &result) {
		UnaryExecutor::Execute<string_t, double>(args.data[0], result, args.size(),
			[&](const string_t &input) {

				double total_area = 0.0;
				BinaryReader reader(input.GetData(), input.GetSize());
				while (!reader.IsAtEnd()) {
					auto meta = reader.Read<BKBMeta>();
					meta.Verify();

					switch (meta.GetType()) {
						case GeometryType::POINT:
						case GeometryType::LINESTRING: {
							const auto vertex_count = meta.GetCount();
							const auto vertex_width = (2 + meta.HasZ() + meta.HasM()) * sizeof(double);
							reader.Skip(vertex_count * vertex_width);
						} break;
						case GeometryType::POLYGON: {
							const auto ring_count = meta.GetCount();
							for (uint32_t ring_idx = 0; ring_idx < ring_count; ring_idx++) {
								const auto ring_meta = reader.Read<BKBMeta>();
								ring_meta.Verify();
								const auto vertex_count = ring_meta.GetCount();
								const auto vertex_width = (2 + ring_meta.HasZ() + ring_meta.HasM()) * sizeof(double);
								const auto vertex_array = reader.Reserve(vertex_count * vertex_width);

								double sum = 0.0;

								if (reader.IsAligned()) {
									const auto vertex_ptr = reinterpret_cast<const VertexXYM*>(vertex_array);
									for (uint32_t vertex_idx = 0; vertex_idx < vertex_count - 1; vertex_idx++) {
										const auto &v1 = vertex_ptr[vertex_idx];
										const auto &v2 = vertex_ptr[vertex_idx + 1];

										sum += (v1.x * v2.y) - (v2.x * v1.y);
									}
								} else {
									for (uint32_t vertex_idx = 0; vertex_idx < vertex_count - 1; vertex_idx++) {
										 VertexXYZM v1;
										 VertexXYZM v2;
										 memcpy(&v1, vertex_array + vertex_idx * vertex_width, sizeof(VertexXY));
										 memcpy(&v2, vertex_array + (vertex_idx + 1) * vertex_width, sizeof(VertexXY));

										 sum += (v1.x * v2.y) - (v2.x * v1.y);
									 }
								}

								sum = std::abs(sum) * 0.5;
								if (ring_idx == 0) {
									total_area += sum; // Add the area of the first ring
								} else {
									total_area -= sum; // Subtract the area of subsequent rings
								}
							}
						} break;
						case GeometryType::MULTIPOINT:
						case GeometryType::MULTILINESTRING:
						case GeometryType::MULTIPOLYGON:
						case GeometryType::GEOMETRYCOLLECTION:
							break;
						default:
							throw InvalidInputException("GeometryVisitor: Unknown meta type %d", static_cast<int>(meta.GetType()));
					}
				}

				return total_area;
		});
	}

	struct AreaVisitor : GeometryVisitor<AreaVisitor> {
		double total_area = 0.0;
		bool in_ring = false;
		uint32_t ring_idx = 0;

		template<class VERTEX_TYPE = VertexXY, bool IS_BIG_ENDIAN = false>
		void Vertices(BinaryReader &reader, uint32_t vertex_count) {
			if (!in_ring) {
				return;
			}

			if (vertex_count < 3) {
				return; // Not enough vertices to form a polygon
			}

			const auto vertex_array = reader.Reserve(vertex_count * sizeof(VERTEX_TYPE));

			double sum = 0.0;
			for (uint32_t i = 0; i < vertex_count - 1; i++) {
				VERTEX_TYPE v1;
				VERTEX_TYPE v2;
				memcpy(&v1, vertex_array + i * sizeof(VERTEX_TYPE), sizeof(VERTEX_TYPE));
				memcpy(&v2, vertex_array + (i + 1) * sizeof(VERTEX_TYPE), sizeof(VERTEX_TYPE));
				sum += (v1.x * v2.y) - (v2.x * v1.y);
			}
			if (ring_idx == 0) {
				total_area += std::abs(sum) * 0.5; // First ring adds area
			} else {
				total_area -= std::abs(sum) * 0.5; // Subsequent rings subtract area
			}
		}

		template<class VERTEX_TYPE>
		void Enter(Tags::Polygon, uint32_t count) {
			// Points do not contribute to area
			ring_idx = 0;
		}

		template<class VERTEX_TYPE>
		void Enter(Tags::Ring, uint32_t count) {
			// Points do not contribute to area
			in_ring = true;
		}

		template<class VERTEX_TYPE>
		void Leave(Tags::Ring, uint32_t count) {
			in_ring = false;
			ring_idx++;
		}

		template<class VERTEX_TYPE>
		void Enter(Tags::Any, uint32_t) { }

		template<class VERTEX_TYPE>
		void Leave(Tags::Any, uint32_t) { }
	};

	static void ExecuteVisitor(DataChunk &args, ExpressionState &state, Vector &result) {
		UnaryExecutor::Execute<string_t, double>(args.data[0], result, args.size(),
			[&](const string_t &input) {
				AreaVisitor visitor;
				visitor.Visit(input.GetData(), input.GetSize());
				return visitor.total_area;
			});
	}

	static void Register(DatabaseInstance &db) {
		ScalarFunctionSet set("st_area");
		set.AddFunction(ScalarFunction({Types::BKB()}, LogicalType::DOUBLE, ExecuteVisitor));
		set.AddFunction(ScalarFunction({Types::WKB()}, LogicalType::DOUBLE, ExecuteVisitor));
		ExtensionUtil::RegisterFunction(db, std::move(set));

		ScalarFunctionSet fast_set("st_area_non_recursive");
		fast_set.AddFunction(ScalarFunction( {Types::BKB()}, LogicalType::DOUBLE, Execute));
		fast_set.AddFunction(ScalarFunction({Types::WKB()}, LogicalType::DOUBLE, ExecuteWKB));
		ExtensionUtil::RegisterFunction(db, std::move(fast_set));
	}
};

struct ST_FlipCoordinates {

	static void ExecuteBKB(DataChunk &args, ExpressionState &state, Vector &result) {

		UnaryExecutor::Execute<string_t, string_t>(
			args.data[0], result, args.size(),
			[&](const string_t &input) {
				BinaryReader reader(input.GetData(), input.GetSize());

				auto blob = StringVector::EmptyString(result, input.GetSize());
				FixedBinaryWriter<false> writer(blob.GetDataWriteable(), blob.GetSize());

				while (!reader.IsAtEnd()) {
					const auto meta = reader.Read<BKBMeta>();
					writer.Write(meta);

					switch (meta.GetType()) {
						case GeometryType::POINT:
						case GeometryType::LINESTRING: {
							const auto vertex_count = meta.GetCount();
							const auto vertex_parts = static_cast<VertexType>(meta.HasZ() + (meta.HasM() * 2));
							switch (vertex_parts) {
								case VertexType::XY: {
									for (uint32_t i = 0; i < vertex_count; i++) {
										auto v = reader.Read<VertexXY>();
										std::swap(v.x, v.y);
										writer.Write(v);
									}
								} break;
								case VertexType::XYM: {
									for (uint32_t i = 0; i < vertex_count; i++) {
										auto v = reader.Read<VertexXYM>();
										std::swap(v.x, v.y);
										writer.Write(v);
									}
								} break;
								case VertexType::XYZ: {
									for (uint32_t i = 0; i < vertex_count; i++) {
										auto v = reader.Read<VertexXYZ>();
										std::swap(v.x, v.y);
										writer.Write(v);
									}
								} break;
								case VertexType::XYZM: {
									for (uint32_t i = 0; i < vertex_count; i++) {
										auto v = reader.Read<VertexXYZM>();
										std::swap(v.x, v.y);
										writer.Write(v);
									}
								} break;
								default:
									throw InvalidInputException("GeometryVisitor: Unknown vertex type %d", static_cast<int>(vertex_parts));
							}
						} break;
						case GeometryType::POLYGON: {
						case GeometryType::MULTIPOINT:
						case GeometryType::MULTILINESTRING:
						case GeometryType::MULTIPOLYGON:
						case GeometryType::GEOMETRYCOLLECTION: {
							continue;
						}
					} break;
					default:
						throw InvalidInputException("GeometryVisitor: Unknown meta type %d", static_cast<int>(meta.GetType()));
				}
			}

			blob.Finalize();
			return blob;
		});
	}

	static void ExecuteWKB(DataChunk &args, ExpressionState &state, Vector &result) {
		UnaryExecutor::Execute<string_t, string_t>(
			args.data[0], result, args.size(),
			[&](const string_t &input) {
				BinaryReader reader(input.GetData(), input.GetSize());

				auto blob = StringVector::EmptyString(result, input.GetSize());
				FixedBinaryWriter<false> writer(blob.GetDataWriteable(), blob.GetSize());

				while (!reader.IsAtEnd()) {

					const auto meta = reader.Read<WKBMeta>();
					writer.Write(meta);

					const auto type_id = meta.GetType();
					const auto type = static_cast<GeometryType>(type_id % 1000);
					const auto flag = type_id / 1000;
					const auto has_z = (flag & 0x01) != 0;
					const auto has_m = (flag & 0x02) != 0;

					switch (type) {
					case GeometryType::POINT: {
						const auto vertex_parts = static_cast<VertexType>(has_z + (has_m * 2));
						switch (vertex_parts) {
							case VertexType::XY: {
								auto v = reader.Read<VertexXY>();
								std::swap(v.x, v.y);
								writer.Write(v);
							} break;
							case VertexType::XYM: {
								auto v = reader.Read<VertexXYM>();
								std::swap(v.x, v.y);
								writer.Write(v);
							} break;
							case VertexType::XYZ: {
								auto v = reader.Read<VertexXYZ>();
								std::swap(v.x, v.y);
								writer.Write(v);
							} break;
							case VertexType::XYZM: {
								auto v = reader.Read<VertexXYZM>();
								std::swap(v.x, v.y);
								writer.Write(v);
							} break;
							default:
								throw InvalidInputException("GeometryVisitor: Unknown vertex type %d", static_cast<int>(vertex_parts));
						}
					} break;
					case GeometryType::LINESTRING: {
						const auto vertex_count = reader.Read<uint32_t>();
						const auto vertex_parts = static_cast<VertexType>(has_z + (has_m * 2));

						writer.Write<uint32_t>(vertex_count);
						switch (vertex_parts) {
							case VertexType::XY: {
								for (uint32_t i = 0; i < vertex_count; i++) {
									auto v = reader.Read<VertexXY>();
									std::swap(v.x, v.y);
									writer.Write(v);
								}
							} break;
							case VertexType::XYM: {
								for (uint32_t i = 0; i < vertex_count; i++) {
									auto v = reader.Read<VertexXYM>();
									std::swap(v.x, v.y);
									writer.Write(v);
								}
							} break;
							case VertexType::XYZ: {
								for (uint32_t i = 0; i < vertex_count; i++) {
									auto v = reader.Read<VertexXYZ>();
									std::swap(v.x, v.y);
									writer.Write(v);
								}
							} break;
							case VertexType::XYZM: {
								for (uint32_t i = 0; i < vertex_count; i++) {
									auto v = reader.Read<VertexXYZM>();
									std::swap(v.x, v.y);
									writer.Write(v);
								}
							} break;
							default:
								throw InvalidInputException("GeometryVisitor: Unknown vertex type %d", static_cast<int>(vertex_parts));
						}
					} break;
					case GeometryType::POLYGON: {
						const auto ring_count = reader.Read<uint32_t>();
						writer.Write<uint32_t>(ring_count);
						for (uint32_t r = 0; r < ring_count; r++) {
							const auto vertex_count = reader.Read<uint32_t>();
							writer.Write<uint32_t>(vertex_count);
							const auto vertex_parts = static_cast<VertexType>(has_z + (has_m * 2));
							switch (vertex_parts) {
								case VertexType::XY: {
									for (uint32_t i = 0; i < vertex_count; i++) {
										auto v = reader.Read<VertexXY>();
										std::swap(v.x, v.y);
										writer.Write(v);
									}
								} break;
								case VertexType::XYM: {
									for (uint32_t i = 0; i < vertex_count; i++) {
										auto v = reader.Read<VertexXYM>();
										std::swap(v.x, v.y);
										writer.Write(v);
									}
								} break;
								case VertexType::XYZ: {
									for (uint32_t i = 0; i < vertex_count; i++) {
										auto v = reader.Read<VertexXYZ>();
										std::swap(v.x, v.y);
										writer.Write(v);
									}
								} break;
								case VertexType::XYZM: {
									for (uint32_t i = 0; i < vertex_count; i++) {
										auto v = reader.Read<VertexXYZM>();
										std::swap(v.x, v.y);
										writer.Write(v);
									}
								} break;
								default:
									throw InvalidInputException("GeometryVisitor: Unknown vertex type %d", static_cast<int>(vertex_parts));
							}
						}
					}
					break;
					case GeometryType::MULTIPOINT:
					case GeometryType::MULTILINESTRING:
					case GeometryType::MULTIPOLYGON:
					case GeometryType::GEOMETRYCOLLECTION: {
						writer.Write<uint32_t>(reader.Read<uint32_t>()); // Write the count, but ignore the geometries
					} break;
					default:
						throw InvalidInputException("GeometryVisitor: Unknown meta type %d", static_cast<int>(type));
					}
				}
				blob.Finalize();
				return blob;
			});
	}

	static void Register(DatabaseInstance &db) {
		ScalarFunctionSet set("st_flip");
		set.AddFunction(ScalarFunction({Types::BKB()}, Types::BKB(), ExecuteBKB));
		set.AddFunction(ScalarFunction({Types::WKB()}, Types::WKB(), ExecuteWKB));
		ExtensionUtil::RegisterFunction(db, std::move(set));
	}

};

} // namespace
} // namespace duckdb
//======================================================================================================================
// Extension Loading
//======================================================================================================================
namespace duckdb {

static void LoadInternal(DatabaseInstance &instance) {
	Types::Register(instance);
	WKB_FromBlob::Register(instance);
	BKB_FromBlob::Register(instance);
	ST_Extent::Register(instance);
	ST_AsWKB::Register(instance);
	ST_Area::Register(instance);
	ST_FlipCoordinates::Register(instance);
}

void GeobenchExtension::Load(DuckDB &db) {
	LoadInternal(*db.instance);
}
std::string GeobenchExtension::Name() {
	return "geobench";
}

std::string GeobenchExtension::Version() const {
#ifdef EXT_VERSION_GEOBENCH
	return EXT_VERSION_GEOBENCH;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_EXTENSION_API void geobench_init(duckdb::DatabaseInstance &db) {
	duckdb::DuckDB db_wrapper(db);
	db_wrapper.LoadExtension<duckdb::GeobenchExtension>();
}

DUCKDB_EXTENSION_API const char *geobench_version() {
	return duckdb::DuckDB::LibraryVersion();
}
}

#ifndef DUCKDB_EXTENSION_MAIN
#error DUCKDB_EXTENSION_MAIN not defined
#endif
