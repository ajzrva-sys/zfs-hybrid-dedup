// Oracle scaffolding only: allocation, assertions, formatting, configuration
// enum dependencies. No chunking or hashing algorithm is implemented here.
using System;
using System.Linq;
namespace System.Diagnostics.ContractsLight {
    public static class Contract { public static void Assert(bool x) { if(!x) throw new InvalidOperationException("Contract assertion"); } }
}
namespace BuildXL.Cache.ContentStore.Interfaces.Utils {
    public static class StringExt { public static string ToHex(this byte[] b) => Convert.ToHexString(b); }
}
namespace BuildXL.Cache.ContentStore.Hashing {
    public enum NodeAlgorithmId { Node64K,Node1024K }
    public enum HashType { Dedup64K,Dedup1024K }
    public static class HashTypeExt { public static int GetAvgChunkSize(this HashType t) => t==HashType.Dedup64K?65536:1048576; }
    public static class ByteArrayComparer { public static bool ArraysEqual(byte[] a,byte[] b) => a.SequenceEqual(b); }
    public interface IHashAlgorithmBufferPool { Pool<byte[]>.PoolHandle GetBufferFromPool(); }
    public class Pool<T> {
        readonly Func<T> create;
        public Pool(Func<T> create,Action<T> reset) { this.create=create; }
        public PoolHandle Get() => new PoolHandle(create());
        public class PoolHandle : IDisposable { public T Value; public PoolHandle(T x) { Value=x; } public void Dispose(){} }
    }
    public class ByteArrayPool : Pool<byte[]> { public ByteArrayPool(int size,int maxReserveInstances):base(()=>new byte[size],x=>{}){} }
}
