using System.Net;
using System.Net.Sockets;
using System.Buffers.Binary;
using UnifiedPicoModule.Sdk;
var server=new TcpListener(IPAddress.Loopback,19032);server.Start();
using var r=new TongueReceiver(IPAddress.Loopback,Console.WriteLine,Path.Combine(AppContext.BaseDirectory,"test-status.json"),19032);
using var peer=server.AcceptTcpClient();Thread.Sleep(20);var stream=peer.GetStream();
byte[] p=new byte[24];"TNG1"u8.CopyTo(p);
void Make(float l,float rr,float o,float c=.8f){BinaryPrimitives.WriteUInt32LittleEndian(p.AsSpan(4),1);BinaryPrimitives.WriteSingleLittleEndian(p.AsSpan(8),l);BinaryPrimitives.WriteSingleLittleEndian(p.AsSpan(12),rr);BinaryPrimitives.WriteSingleLittleEndian(p.AsSpan(16),o);BinaryPrimitives.WriteSingleLittleEndian(p.AsSpan(20),c);}
if(r.Poll().Fresh)throw new Exception("fresh before data");
Make(.8f,.1f,1);stream.Write(p,0,7);Thread.Sleep(15);if(r.Poll().Fresh)throw new Exception("partial accepted");stream.Write(p,7,17);Thread.Sleep(15);var t=r.Poll();if(!t.Fresh||t.Left!=.8f||t.Right!=.1f||t.Out!=1)throw new Exception("valid decode");
t=r.Poll(0);if(t.Left!=.8f||t.Out!=1)throw new Exception("high-confidence fallback failed");
Make(.8f,.1f,0,0);stream.Write(p);Thread.Sleep(10);
t=r.Poll(0);if(t.Left!=0||t.Right!=0||t.Out!=0)throw new Exception("negative confidence reset failed");
t=r.Poll(.125f);if(Math.Abs(t.Left-.4f)>.001f)throw new Exception("fractional native gate failed");
Make(float.NaN,1,1);stream.Write(p);Thread.Sleep(10);if(r.Poll().Left!=.8f)throw new Exception("NaN accepted");
p[0]=0;Make(0,1,1);stream.Write(p);Thread.Sleep(10);if(r.Poll().Left!=.8f)throw new Exception("bad magic accepted");
Thread.Sleep(400);t=r.Poll();if(t.Fresh||t.Left!=0||t.Right!=0||t.Out!=0)throw new Exception("stale not neutral");
server.Stop();Console.WriteLine("PASS:TCP fragmentation,invalid/NaN rejection,high-confidence fallback,negative reset,fractional gate,350ms stale neutralization");
