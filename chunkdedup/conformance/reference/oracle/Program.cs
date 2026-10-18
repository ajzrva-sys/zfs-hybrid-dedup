using System;
using System.IO;
using BuildXL.Cache.ContentStore.Hashing;
class Program {
    static void Fill(int seed,byte[] bytes) {
        if(seed<0)return;
        var r=new Random(seed);r.NextBytes(bytes);
        int start=r.Next(bytes.Length),end=r.Next(start,bytes.Length);
        for(int i=start;i<end;i++)bytes[i]=0;
    }
    static int Main(string[] args) {
        if(args.Length<1)return 2;
        if(args[0]=="fixture") {
            int seed=int.Parse(args[1]),size=int.Parse(args[2]);
            var bytes=new byte[size];Fill(seed,bytes);File.WriteAllBytes(args[3],bytes);return 0;
        }
        if(args[0]=="raw") {
            var bytes=File.ReadAllBytes(args[1]);
            var regression=new BuildXL.Cache.ContentStore.Hashing.Chunking.RegressionChunking(new ChunkerConfiguration(65536),c=> {
                var hash=System.Security.Cryptography.SHA512.HashData(bytes.AsSpan((int)c.m_nStartChunk,(int)c.m_nChunkLength));
                Console.WriteLine($"{c.m_nStartChunk}\t{c.m_nChunkLength}\t{Convert.ToHexString(hash.AsSpan(0,32)).ToLowerInvariant()}\t{(int)c.m_eCutType}");
            });
            regression.PushBuffer(bytes);regression.Complete();return 0;
        }
        int block=args.Length>1?int.Parse(args[1]):262144;
        using var file=args[0]=="-"?Console.OpenStandardInput():File.OpenRead(args[0]);
        var input=new byte[block];
        using var chunker=new ManagedChunker(new ChunkerConfiguration(65536));
        using var session=chunker.BeginChunking(c=>Console.WriteLine($"{c.Offset}\t{c.Size}\t{Convert.ToHexString(c.Hash).ToLowerInvariant()}"));
        int count;
        while((count=file.Read(input,0,input.Length))>0)session.PushBuffer(input,0,count);
        return 0;
    }
}
