using System;
using System.IO;
using System.Net;
using System.Net.Sockets;
using Net = System.Net;
using Sock = System.Net.Sockets.Socket;

namespace Tamir.SharpSsh.java.net
{
	/// <summary>
	/// Summary description for Socket.
	/// </summary>
	public class Socket
	{
		internal Sock sock;
	
		protected void SetSocketOption(SocketOptionLevel level, SocketOptionName name, int val)
		{
			try
			{
				sock.SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.NoDelay, val);
			}
			catch
			{
			}
		}

//		public Socket(AddressFamily af, SocketType st, ProtocolType pt)
//		{
//			this.sock = new Sock(af, st, pt);
//			this.sock.Connect();
//		}

		public Socket(string host, int port)
		{	
			IPEndPoint ep = new IPEndPoint(Dns.GetHostByName(host).AddressList[0], port);
			this.sock = new Sock(ep.AddressFamily,
			SocketType.Stream, ProtocolType.Tcp);
			this.sock.Connect(ep);
		}

		public Socket(Sock sock)
		{	
			this.sock = sock;
		}

		public Stream getInputStream()
		{
			return new Net.Sockets.NetworkStream(sock);
		}

		public Stream getOutputStream()
		{
			return new Net.Sockets.NetworkStream(sock);
		}

		public bool isConnected()
		{
			return sock.Connected;
		}

		public void setTcpNoDelay(bool b)
		{
			if(b)
			{
				SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.NoDelay, 1);
			}
			else
			{
				SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.NoDelay, 0);
			}
		}

		public void setSoTimeout(int t)
		{
			SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.ReceiveTimeout, t);
			SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.SendTimeout, t);
		}

		public void close()
		{
			sock.Close();
		}

		public InetAddress getInetAddress()
		{
			return new InetAddress( ((IPEndPoint) sock.RemoteEndPoint).Address );
		}

		public int getPort()
		{
			return ((IPEndPoint) sock.RemoteEndPoint).Port;
		}
	}
}
