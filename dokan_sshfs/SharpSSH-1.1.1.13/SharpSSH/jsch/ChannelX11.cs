using System;
using System.Net;
using System.Net.Sockets;
using Tamir.SharpSsh.java.lang;

namespace Tamir.SharpSsh.jsch
{
	/* -*-mode:java; c-basic-offset:2; -*- */
	/*
	Copyright (c) 2002,2003,2004 ymnk, JCraft,Inc. All rights reserved.

	Redistribution and use in source and binary forms, with or without
	modification, are permitted provided that the following conditions are met:

	  1. Redistributions of source code must retain the above copyright notice,
		 this list of conditions and the following disclaimer.

	  2. Redistributions in binary form must reproduce the above copyright 
		 notice, this list of conditions and the following disclaimer in 
		 the documentation and/or other materials provided with the distribution.

	  3. The names of the authors may not be used to endorse or promote products
		 derived from this software without specific prior written permission.

	THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESSED OR IMPLIED WARRANTIES,
	INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
	FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL JCRAFT,
	INC. OR ANY CONTRIBUTORS TO THIS SOFTWARE BE LIABLE FOR ANY DIRECT, INDIRECT,
	INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
	LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,
	OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
	LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
	NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
	EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
	*/
	internal class ChannelX11 : Channel
	{

		private const int LOCAL_WINDOW_SIZE_MAX=0x20000;
		private const int LOCAL_MAXIMUM_PACKET_SIZE=0x4000;

		internal static String host="127.0.0.1";
		internal static int port=6000;

		internal bool _init=true;

		internal static byte[] cookie=null;
		//static byte[] cookie_hex="0c281f065158632a427d3e074d79265d".getBytes();
		internal static byte[] cookie_hex=null;

		private static System.Collections.Hashtable faked_cookie_pool=new System.Collections.Hashtable();
		private static System.Collections.Hashtable faked_cookie_hex_pool=new System.Collections.Hashtable();

		internal static byte[] table={0x30,0x31,0x32,0x33,0x34,0x35,0x36,0x37,0x38,0x39,
										 0x61,0x62,0x63,0x64,0x65,0x66};
		internal static int revtable(byte foo)
		{
			for(int i=0; i<table.Length; i++)
			{
				if(table[i]==foo)return i;
			}
			return 0;
		}
		internal static void setCookie(String foo)
		{
			cookie_hex=Util.getBytes(foo); 
			cookie=new byte[16];
			for(int i=0; i<16; i++)
			{
				cookie[i]=(byte)(((revtable(cookie_hex[i*2])<<4)&0xf0) |
					((revtable(cookie_hex[i*2+1]))&0xf));
			}
		}
		internal static void setHost(String foo){ host=foo; }
		internal static void setPort(int foo){ port=foo; }
		internal static byte[] getFakedCookie(Session session)
		{
			lock(faked_cookie_hex_pool)
			{
				byte[] foo=(byte[])faked_cookie_hex_pool[session];
				if(foo==null)
				{
					Random random=Session.random;
					foo=new byte[16];
					lock(random)
					{
						random.fill(foo, 0, 16);
					}
					/*
					System.out.print("faked_cookie: ");
					for(int i=0; i<foo.length; i++){
						System.out.print(Integer.toHexString(foo[i]&0xff)+":");
					}
					System.out.println("");
					*/
					faked_cookie_pool.Add(session, foo);
					byte[] bar=new byte[32];
					for(int i=0; i<16; i++)
					{
						bar[2*i]=table[(foo[i]>>4)&0xf];
						bar[2*i+1]=table[(foo[i])&0xf];
					}
					faked_cookie_hex_pool.Add(session, bar);
					foo=bar;
				}
				return foo;
			}
		}

		Socket socket = null;
		internal ChannelX11():base()
		{
    
			setLocalWindowSizeMax(LOCAL_WINDOW_SIZE_MAX);
			setLocalWindowSize(LOCAL_WINDOW_SIZE_MAX);
			setLocalPacketSize(LOCAL_MAXIMUM_PACKET_SIZE);

			type=Util.getBytes("x11");
			try
			{ 
				IPEndPoint ep = new IPEndPoint(Dns.GetHostByName(host).AddressList[0], port);		
				socket=new Socket(ep.AddressFamily, SocketType.Stream, ProtocolType.Tcp);
				socket.SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.NoDelay, 1);
				socket.Connect(ep);
				io=new IO();
				NetworkStream ns = new NetworkStream( socket );
				io.setInputStream(ns);
				io.setOutputStream(ns);
			}
			catch(Exception e)
			{
				Console.WriteLine(e);
			}
		}

		public override void run()
		{
			thread=Thread.currentThread();
			Buffer buf=new Buffer(rmpsize);
			Packet packet=new Packet(buf);
			int i=0;
			try
			{
				while(thread!=null)
				{
					i=io.ins.Read(buf.buffer, 
						14, 
						buf.buffer.Length-14
						-16 -20 // padding and mac
						);
					if(i<=0)
					{
						eof();
						break;
					}
					if(_close)break;
					packet.reset();
					buf.putByte((byte)Session.SSH_MSG_CHANNEL_DATA);
					buf.putInt(recipient);
					buf.putInt(i);
					buf.skip(i);
					session.write(packet, this, i);
				}
			}
			catch
			{
				//System.out.println(e);
			}
			thread=null;
		}

		internal override void write(byte[] foo, int s, int l) 
		{
			//if(eof_local)return;

			if(_init)
			{
				int plen=(foo[s+6]&0xff)*256+(foo[s+7]&0xff);
				int dlen=(foo[s+8]&0xff)*256+(foo[s+9]&0xff);
				if((foo[s]&0xff)==0x42)
				{
				}
				else if((foo[s]&0xff)==0x6c)
				{
					plen=(int)(((uint)plen>>8)&0xff)|((plen<<8)&0xff00);
					dlen=(int)(((uint)dlen>>8)&0xff)|((dlen<<8)&0xff00);
				}
				else
				{
					// ??
				}
				byte[] bar=new byte[dlen];
				Array.Copy(foo, s+12+plen+((-plen)&3), bar, 0, dlen);
				byte[] faked_cookie=(byte[])faked_cookie_pool[session];

				if(equals(bar, faked_cookie))
				{
					if(cookie!=null)
						Array.Copy(cookie, 0, foo, s+12+plen+((-plen)&3), dlen);
				}
				else
				{
					Console.WriteLine("wrong cookie");
				}
				_init=false;
			}
			io.put(foo, s, l);
		}

		public override void disconnect()
		{
			close();
			thread=null;
			try
			{
				if(io!=null)
				{
					try
					{
						if(io.ins!=null)
							io.ins.Close();
					}
					catch{}
					try
					{
						if(io.outs!=null)
							io.outs.Close();
					}
					catch{}
				}
				try
				{
					if(socket!=null)
						socket.Close();
				}
				catch{}
			}
			catch(Exception e)
			{
				Console.WriteLine(e.StackTrace);
			}
			io=null;
			Channel.del(this);
		}

		private static bool equals(byte[] foo, byte[] bar)
		{
			if(foo.Length!=bar.Length)return false;
			for(int i=0; i<foo.Length; i++)
			{
				if(foo[i]!=bar[i])return false;
			}
			return true;
		}
	}

}
