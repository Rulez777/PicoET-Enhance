using System.Buffers.Binary;
using System.Diagnostics;
using System.Net;
using System.Net.Sockets;
namespace UnifiedPicoModule.Sdk;
// Loopback TCP forwarded by authenticated ADB to the headset's abstract Unix socket.
internal sealed class TongueReceiver : IDisposable
{
    private TcpClient socket=new();
    private Task connect;
    private long lastConnect;
    private readonly int port;
    private readonly Action<string> log;
    private readonly string statusPath;
    private readonly byte[] buffer=new byte[24];
    private int buffered;
    private long received,reported;
    private uint sequence;
    private float left,right,tongueOut,confidence;
    private bool fallbackActive,negative,uncertain;
    private long lastVisual;
    private float heldLeft,heldRight,heldOut;
    public TongueReceiver(IPAddress source,Action<string> logger,string diagnosticPath, int localPort=19031)
    {
        log=logger;statusPath=diagnosticPath;port=localPort;socket.NoDelay=true;
        connect=socket.ConnectAsync(IPAddress.Loopback,port);lastConnect=Stopwatch.GetTimestamp();
        log("Tongue experiment: authenticated ADB loopback TCP19031; timeout350ms.");
    }
    public (bool Fresh,float Left,float Right,float Out) Poll(float stockTongueOut=1)
    {
        if((connect.IsFaulted || (connect.IsCompletedSuccessfully && (!socket.Connected || (socket.Client.Poll(0,SelectMode.SelectRead)&&socket.Available==0)))) && Stopwatch.GetElapsedTime(lastConnect).TotalSeconds>1)
        {
            _=connect.Exception;socket.Dispose();socket=new TcpClient{NoDelay=true};buffered=0;
            connect=socket.ConnectAsync(IPAddress.Loopback,port);lastConnect=Stopwatch.GetTimestamp();
        }
        if(connect.IsCompletedSuccessfully && socket.Connected)
        {
            for(int i=0;i<64 && socket.Available>0;i++)
            {
                int n=socket.GetStream().Read(buffer,buffered,Math.Min(24-buffered,socket.Available));
                if(n<=0)break;buffered+=n;if(buffered<24)continue;buffered=0;
                var p=buffer;
                if(!p.AsSpan(0,4).SequenceEqual("TNG2"u8))continue;
                float l=BinaryPrimitives.ReadSingleLittleEndian(p.AsSpan(8)),r=BinaryPrimitives.ReadSingleLittleEndian(p.AsSpan(12));
                float o=BinaryPrimitives.ReadSingleLittleEndian(p.AsSpan(16)),c=BinaryPrimitives.ReadSingleLittleEndian(p.AsSpan(20));
                if(!float.IsFinite(l)||!float.IsFinite(r)||!float.IsFinite(o)||!float.IsFinite(c)||l<0||l>1||r<0||r>1||o<0||o>1||c<0||c>1)continue;
                left=l;right=r;tongueOut=o;confidence=c;uint bits=BinaryPrimitives.ReadUInt32LittleEndian(p.AsSpan(4));sequence=bits&0x3fffffff;
                negative=(bits&0x80000000)!=0;uncertain=(bits&0x40000000)!=0;received=Stopwatch.GetTimestamp();
                if(!negative&&!uncertain&&o>.5f&&c>.15f){lastVisual=received;heldLeft=l;heldRight=r;heldOut=o;}
            }
        }
        long now=Stopwatch.GetTimestamp();
        bool fresh=received!=0&&Stopwatch.GetElapsedTime(received,now).TotalMilliseconds<350;
        float native=float.IsFinite(stockTongueOut)?Math.Clamp(stockTongueOut,0,1):0;
        bool held=fresh&&!negative&&lastVisual!=0&&Stopwatch.GetElapsedTime(lastVisual,now).TotalMilliseconds<250;
        if(!fresh||negative)fallbackActive=false;
        else if(!uncertain&&confidence>.45f&&tongueOut>.6f)fallbackActive=true;
        else if(!held)fallbackActive=false;
        float fallback=fresh&&fallbackActive?(held?Math.Max(tongueOut,heldOut):tongueOut):0;
        float combined=Math.Max(native,fallback);
        float gate=Math.Clamp((combined-.05f)/.15f,0,1);
        float modelLeft=uncertain&&held?heldLeft:left,modelRight=uncertain&&held?heldRight:right;
        float finalLeft=fresh?modelLeft*gate:0,finalRight=fresh?modelRight*gate:0;
        if(reported==0||Stopwatch.GetElapsedTime(reported,now).TotalSeconds>=1)
        {
            reported=now;
            try{File.WriteAllText(statusPath,System.Text.Json.JsonSerializer.Serialize(new{sequence,fresh,left=finalLeft,right=finalRight,tongueOut=combined,nativeOut=native,confidence,negative,uncertain,held,modelOut=fresh?tongueOut:0,transport="ADB",utc=DateTime.UtcNow}));}catch(IOException){}catch(UnauthorizedAccessException){}
            log($"Tongue experiment seq={sequence} fresh={fresh} L={(fresh?left:0):F3} R={(fresh?right:0):F3}");
        }
        return fresh?(true,finalLeft,finalRight,combined):(false,0,0,0);
    }
    public void Dispose()=>socket.Dispose();
}
