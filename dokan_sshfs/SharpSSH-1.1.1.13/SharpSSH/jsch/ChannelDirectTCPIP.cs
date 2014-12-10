using System;
using System.IO;
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

	public class ChannelDirectTCPIP : Channel
	{

		private const int LOCAL_WINDOW_SIZE_MAX=0x20000;
		private const  int LOCAL_MAXIMUM_PACKET_SIZE=0x4000;

		internal String host;
		internal int port;

		internal String originator_IP_address="127.0.0.1";
		internal int originator_port=0;

		internal ChannelDirectTCPIP() : base()
		{
			setLocalWindowSizeMax(LOCAL_WINDOW_SIZE_MAX);
			setLocalWindowSize(LOCAL_WINDOW_SIZE_MAX);
			setLocalPacketSize(LOCAL_MAXIMUM_PACKET_SIZE);
		}

		public override void init ()
		{
			try
			{ 
				io=new IO();
			}
			catch(Exception e)
			{
				Console.WriteLine(e);
			}
		}

		public override void connect()
		{
			try
			{
				if(!session.isConnected())
				{
					throw new JSchException("session is down");
				}
				Buffer buf=new Buffer(150);
				Packet packet=new Packet(buf);
				// send
				// byte   SSH_MSG_CHANNEL_OPEN(90)
				// string channel type         //
				// uint32 sender channel       // 0
				// uint32 initial window size  // 0x100000(65536)
				// uint32 maxmum packet size   // 0x4000(16384)

				packet.reset();
				buf.putByte((byte)90);
				buf.putString(Util.getBytes("direct-tcpip"));
				buf.putInt(id);
				buf.putInt(lwsize);
				buf.putInt(lmpsize);
				buf.putString(Util.getBytes(host));
				buf.putInt(port);
				buf.putString(Util.getBytes(originator_IP_address));
				buf.putInt(originator_port);
				session.write(packet);

				int retry=1000;
				try
				{
					while(this.getRecipient()==-1 && 
						session.isConnected() &&
						retry>0 &&
						!_eof_remote)
					{
						//Thread.sleep(500);
						Thread.Sleep(50);
						retry--;
					}
				}
				catch
				{
				}

				if(!session.isConnected())
				{
					throw new JSchException("session is down");
				}
				if(retry==0 || this._eof_remote)
				{
					throw new JSchException("channel is not opened.");
				}
				/*
				if(this.eof_remote){      // failed to open
				  disconnect();
				  return;
				}
				*/

				connected=true;

				thread=new Thread(this);
				thread.start();
			}
			catch(Exception e)
			{
				io.close();
				io=null;
				Channel.del(this);
				if (e is JSchException) 
				{
					throw (JSchException) e;
				}
			}
		}

		public override void run()
		{
			//    thread=Thread.currentThread();
			//System.out.println("rmpsize: "+rmpsize+", lmpsize: "+lmpsize);
			Buffer buf=new Buffer(rmpsize);
			//    Buffer buf=new Buffer(lmpsize);
			Packet packet=new Packet(buf);
			int i=0;
			try
			{
				while(isConnected() &&
					thread!=null && 
					io!=null && 
					io.ins!=null)
				{
					i=io.ins.Read(buf.buffer, 
						14, 
						buf.buffer.Length-14
						-32 -20 // padding and mac
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
			}
			disconnect();
			//System.out.println("connect end");

			/*
				try{
				  packet.reset();
				  buf.putByte((byte)Session.SSH_MSG_CHANNEL_EOF);
				  buf.putInt(recipient);
				  session.write(packet);
				}
				catch(Exception e){
				}
			*/
			//    close();
		}

		public override void setInputStream(Stream ins)
		{
			io.setInputStream(ins);
		}
		public override void setOutputStream(Stream outs)
		{
			io.setOutputStream(outs);
		}

		public void setHost(String host){this.host=host;}
		public void setPort(int port){this.port=port;}
		public void setOrgIPAddress(String foo){this.originator_IP_address=foo;}
		public void setOrgPort(int foo){this.originator_port=foo;}
	}

}
