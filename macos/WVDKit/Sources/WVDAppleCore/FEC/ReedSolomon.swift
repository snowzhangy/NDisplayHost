import Foundation

/// GF(2^8) arithmetic, primitive polynomial x^8 + x^4 + x^3 + x^2 + 1 (0x11D).
public enum GF256 {
    static let tables: (exp: [UInt8], log: [UInt8]) = {
        var exp = [UInt8](repeating: 0, count: 512)
        var log = [UInt8](repeating: 0, count: 256)
        var x = 1
        for i in 0..<255 {
            exp[i] = UInt8(x)
            log[x] = UInt8(i)
            x <<= 1
            if x & 0x100 != 0 { x ^= 0x11D }
        }
        for i in 255..<512 { exp[i] = exp[i - 255] }
        return (exp, log)
    }()

    /// Full 256x256 multiplication table (64 KiB), row = coefficient. One lookup per byte in the hot loop.
    static let mulTable: [UInt8] = {
        var t = [UInt8](repeating: 0, count: 65536)
        for a in 1..<256 {
            for b in 1..<256 {
                t[a << 8 | b] = tables.exp[Int(tables.log[a]) + Int(tables.log[b])]
            }
        }
        return t
    }()

    @inline(__always) public static func mul(_ a: UInt8, _ b: UInt8) -> UInt8 {
        if a == 0 || b == 0 { return 0 }
        return tables.exp[Int(tables.log[Int(a)]) + Int(tables.log[Int(b)])]
    }

    @inline(__always) public static func inv(_ a: UInt8) -> UInt8 {
        precondition(a != 0, "GF(256) inverse of zero")
        return tables.exp[255 - Int(tables.log[Int(a)])]
    }

    /// dst ^= c * src over `count` bytes.
    @inline(__always)
    static func mulAdd(_ dst: UnsafeMutablePointer<UInt8>, _ src: UnsafePointer<UInt8>, _ c: UInt8, _ count: Int) {
        if c == 0 { return }
        if c == 1 {
            var i = 0
            let words = count / 8
            let d64 = UnsafeMutableRawPointer(dst)
            let s64 = UnsafeRawPointer(src)
            while i < words {
                let v = d64.loadUnaligned(fromByteOffset: i * 8, as: UInt64.self) ^ s64.loadUnaligned(fromByteOffset: i * 8, as: UInt64.self)
                d64.storeBytes(of: v, toByteOffset: i * 8, as: UInt64.self)
                i += 1
            }
            for j in (words * 8)..<count { dst[j] ^= src[j] }
            return
        }
        mulTable.withUnsafeBufferPointer { t in
            let row = t.baseAddress! + (Int(c) << 8)
            for i in 0..<count { dst[i] ^= row[Int(src[i])] }
        }
    }
}

/// Systematic Cauchy Reed-Solomon erasure code over GF(2^8), `k + m <= 255`.
///
/// Parity row `j` (0..<m), data column `i` (0..<k): `C[j][i] = 1 / ((k + j) XOR i)`. Every square
/// submatrix of a Cauchy matrix is invertible, so any `k` of the `k + m` shards reconstruct the data.
/// Wire-normative (core/proto/WIRE.md §5): the Windows host must use the same matrix.
public enum ReedSolomon {
    public enum RSError: Error, Equatable {
        case tooFewShards(have: Int, need: Int)
        case invalidParameters
    }

    @inline(__always) public static func coefficient(parityRow j: Int, dataColumn i: Int, k: Int) -> UInt8 {
        GF256.inv(UInt8(k + j) ^ UInt8(i))
    }

    /// Computes `parity.count` parity shards from `data`. All shards are `shardBytes` long.
    public static func encode(data: [UnsafePointer<UInt8>], parity: [UnsafeMutablePointer<UInt8>], shardBytes: Int) throws {
        let k = data.count, m = parity.count
        guard k > 0, k + m <= 255 else { throw RSError.invalidParameters }
        for j in 0..<m {
            parity[j].initialize(repeating: 0, count: shardBytes)
            for i in 0..<k {
                GF256.mulAdd(parity[j], data[i], coefficient(parityRow: j, dataColumn: i, k: k), shardBytes)
            }
        }
    }

    /// Reconstructs missing data shards in place.
    /// - Parameters:
    ///   - data: `k` pointers to data shard storage; entries for missing shards are written.
    ///   - present: which data shards are valid.
    ///   - parity: available parity shards as (row index j, pointer).
    public static func reconstruct(k: Int, m: Int, data: [UnsafeMutablePointer<UInt8>], present: [Bool],
                                   parity: [(row: Int, shard: UnsafePointer<UInt8>)], shardBytes: Int) throws {
        guard k > 0, k + m <= 255, data.count == k, present.count == k else { throw RSError.invalidParameters }
        let missing = (0..<k).filter { !present[$0] }
        let d = missing.count
        if d == 0 { return }
        guard parity.count >= d else { throw RSError.tooFewShards(have: k - d + parity.count, need: k) }
        let used = Array(parity.prefix(d))

        // 1. Remove the known data contributions from each used parity shard.
        let scratch = UnsafeMutablePointer<UInt8>.allocate(capacity: d * shardBytes)
        defer { scratch.deallocate() }
        for (r, p) in used.enumerated() {
            let dst = scratch + r * shardBytes
            dst.update(from: p.shard, count: shardBytes)
            for i in 0..<k where present[i] {
                GF256.mulAdd(dst, data[i], coefficient(parityRow: p.row, dataColumn: i, k: k), shardBytes)
            }
        }

        // 2. Invert the d x d Cauchy submatrix M[r][c] = C[row_r][missing_c] (Gauss-Jordan).
        var a = [UInt8](repeating: 0, count: d * d)
        var inv = [UInt8](repeating: 0, count: d * d)
        for r in 0..<d {
            for c in 0..<d { a[r * d + c] = coefficient(parityRow: used[r].row, dataColumn: missing[c], k: k) }
            inv[r * d + r] = 1
        }
        for col in 0..<d {
            guard let pivot = (col..<d).first(where: { a[$0 * d + col] != 0 }) else { throw RSError.invalidParameters }
            if pivot != col {
                for c in 0..<d {
                    a.swapAt(pivot * d + c, col * d + c)
                    inv.swapAt(pivot * d + c, col * d + c)
                }
            }
            let pinv = GF256.inv(a[col * d + col])
            for c in 0..<d {
                a[col * d + c] = GF256.mul(a[col * d + c], pinv)
                inv[col * d + c] = GF256.mul(inv[col * d + c], pinv)
            }
            for r in 0..<d where r != col {
                let f = a[r * d + col]
                if f == 0 { continue }
                for c in 0..<d {
                    a[r * d + c] ^= GF256.mul(f, a[col * d + c])
                    inv[r * d + c] ^= GF256.mul(f, inv[col * d + c])
                }
            }
        }

        // 3. missing_c = sum_r inv[c][r] * scratch_r
        for (c, idx) in missing.enumerated() {
            let dst = data[idx]
            dst.initialize(repeating: 0, count: shardBytes)
            for r in 0..<d {
                GF256.mulAdd(dst, scratch + r * shardBytes, inv[c * d + r], shardBytes)
            }
        }
    }
}
