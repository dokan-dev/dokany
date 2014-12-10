using System;

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


	public class DHGEX : KeyExchange
	{

		internal const int SSH_MSG_KEX_DH_GEX_GROUP=               31;
		internal const int SSH_MSG_KEX_DH_GEX_INIT=                32;
		internal const int SSH_MSG_KEX_DH_GEX_REPLY=               33;

		internal static int min=1024;

		//  static int min=512;
		internal static int preferred=1024;
		internal static int max=1024;

		//  static int preferred=1024;
		//  static int max=2000;

		internal const int RSA=0;
		internal const int DSS=1;
		private int type=0;

		private int state;

		//  com.jcraft.jsch.DH dh;
		internal DH dh;

		internal byte[] V_S;
		internal byte[] V_C;
		internal byte[] I_S;
		internal byte[] I_C;

		private Buffer buf;
		private Packet packet;

		private byte[] p;
		private byte[] g;
		private byte[] e;
		//private byte[] f;

		public override void init(Session session,
			byte[] V_S, byte[] V_C, byte[] I_S, byte[] I_C)
		{
			this.session=session;
			this.V_S=V_S;      
			this.V_C=V_C;      
			this.I_S=I_S;      
			this.I_C=I_C;      

			//    sha=new SHA1();
			//    sha.init();

			try
			{
				Type t=Type.GetType(session.getConfig("sha-1"));
				sha=(HASH)(Activator.CreateInstance(t));
				sha.init();
			}
			catch(Exception e)
			{
				Console.WriteLine(e);
			}

			buf=new Buffer();
			packet=new Packet(buf);

			try
			{
				Type t=Type.GetType(session.getConfig("dh"));
				dh=(DH)(Activator.CreateInstance(t));
				dh.init();
			}
			catch(Exception e)
			{
				throw e;
			}

			packet.reset();
			buf.putByte((byte)0x22);
			buf.putInt(min);
			buf.putInt(preferred);
			buf.putInt(max);
			session.write(packet); 

			state=SSH_MSG_KEX_DH_GEX_GROUP;
		}

		public override bool next(Buffer _buf) 
		{
			int i,j;
			bool result=false;
			switch(state)
			{
				case SSH_MSG_KEX_DH_GEX_GROUP:
					// byte  SSH_MSG_KEX_DH_GEX_GROUP(31)
					// mpint p, safe prime
					// mpint g, generator for subgroup in GF (p)
					_buf.getInt();
					_buf.getByte();
					j=_buf.getByte();
					if(j!=31)
					{
						Console.WriteLine("type: must be 31 "+j);
						result = false;
					}

					p=_buf.getMPInt();
					g=_buf.getMPInt();
					/*
			  for(int iii=0; iii<p.length; iii++){
			  System.out.println("0x"+Integer.toHexString(p[iii]&0xff)+",");
			  }
			  System.out.println("");
			  for(int iii=0; iii<g.length; iii++){
			  System.out.println("0x"+Integer.toHexString(g[iii]&0xff)+",");
			  }
					*/
					dh.setP(p);
					dh.setG(g);

					// The client responds with:
					// byte  SSH_MSG_KEX_DH_GEX_INIT(32)
					// mpint e <- g^x mod p
					//         x is a random number (1 < x < (p-1)/2)

					e=dh.getE();

					packet.reset();
					buf.putByte((byte)0x20);
					buf.putMPInt(e);
					session.write(packet);

					state=SSH_MSG_KEX_DH_GEX_REPLY;
					result = true;
					break;

				case SSH_MSG_KEX_DH_GEX_REPLY:
					// The server responds with:
					// byte      SSH_MSG_KEX_DH_GEX_REPLY(33)
					// string    server public host key and certificates (K_S)
					// mpint     f
					// string    signature of H
					j=_buf.getInt();
					j=_buf.getByte();
					j=_buf.getByte();
					if(j!=33)
					{
						Console.WriteLine("type: must be 33 "+j);
						result = false;
					}

					K_S=_buf.getString();
					// K_S is server_key_blob, which includes ....
					// string ssh-dss
					// impint p of dsa
					// impint q of dsa
					// impint g of dsa
					// impint pub_key of dsa
					//System.out.print("K_S: "); dump(K_S, 0, K_S.length);

					byte[] f=_buf.getMPInt();
					byte[] sig_of_H=_buf.getString();

					dh.setF(f);
					K=dh.getK();

					//The hash H is computed as the HASH hash of the concatenation of the
					//following:
					// string    V_C, the client's version string (CR and NL excluded)
					// string    V_S, the server's version string (CR and NL excluded)
					// string    I_C, the payload of the client's SSH_MSG_KEXINIT
					// string    I_S, the payload of the server's SSH_MSG_KEXINIT
					// string    K_S, the host key
					// uint32    min, minimal size in bits of an acceptable group
					// uint32   n, preferred size in bits of the group the server should send
					// uint32    max, maximal size in bits of an acceptable group
					// mpint     p, safe prime
					// mpint     g, generator for subgroup
					// mpint     e, exchange value sent by the client
					// mpint     f, exchange value sent by the server
					// mpint     K, the shared secret
					// This value is called the exchange hash, and it is used to authenti-
					// cate the key exchange.

					buf.reset();
					buf.putString(V_C); buf.putString(V_S);
					buf.putString(I_C); buf.putString(I_S);
					buf.putString(K_S);
					buf.putInt(min); buf.putInt(preferred); buf.putInt(max);
					buf.putMPInt(p); buf.putMPInt(g); buf.putMPInt(e); buf.putMPInt(f);
					buf.putMPInt(K);

					byte[] foo=new byte[buf.getLength()];
					buf.getByte(foo);
					sha.update(foo, 0, foo.Length);

					H=sha.digest();

					// System.out.print("H -> "); dump(H, 0, H.length);

					i=0;
					j=0;
					j=(int)((K_S[i++]<<24)&0xff000000)|((K_S[i++]<<16)&0x00ff0000)|
						((K_S[i++]<<8)&0x0000ff00)|((K_S[i++])&0x000000ff);
					String alg=Util.getString(K_S, i, j);
					i+=j;

					
					if(alg.Equals("ssh-rsa"))
					{
						byte[] tmp;
						byte[] ee;
						byte[] n;
	
						type=RSA;

						j=(int)((K_S[i++]<<24)&0xff000000)|((K_S[i++]<<16)&0x00ff0000)|
							((K_S[i++]<<8)&0x0000ff00)|((K_S[i++])&0x000000ff);
						tmp=new byte[j]; Array.Copy(K_S, i, tmp, 0, j); i+=j;
						ee=tmp;
						j=(int)((K_S[i++]<<24)&0xff000000)|((K_S[i++]<<16)&0x00ff0000)|
							((K_S[i++]<<8)&0x0000ff00)|((K_S[i++])&0x000000ff);
						tmp=new byte[j]; Array.Copy(K_S, i, tmp, 0, j); i+=j;
						n=tmp;

						//	SignatureRSA sig=new SignatureRSA();
						//	sig.init();

						SignatureRSA sig=null;
						try
						{
							Type t=Type.GetType(session.getConfig("signature.rsa"));
							sig=(SignatureRSA)(Activator.CreateInstance(t));
							sig.init();
						}
						catch(Exception eee)
						{
							Console.WriteLine(eee);
						}

						sig.setPubKey(ee, n);   
						sig.update(H);
						result=sig.verify(sig_of_H);
					}
					else if(alg.Equals("ssh-dss"))
					{
						byte[] q=null;
						byte[] tmp;

						type=DSS;

						j=(int)((K_S[i++]<<24)&0xff000000)|((K_S[i++]<<16)&0x00ff0000)|
							((K_S[i++]<<8)&0x0000ff00)|((K_S[i++])&0x000000ff);
						tmp=new byte[j]; Array.Copy(K_S, i, tmp, 0, j); i+=j;
						p=tmp;
						j=(int)((K_S[i++]<<24)&0xff000000)|((K_S[i++]<<16)&0x00ff0000)|
							((K_S[i++]<<8)&0x0000ff00)|((K_S[i++])&0x000000ff);
						tmp=new byte[j]; Array.Copy(K_S, i, tmp, 0, j); i+=j;
						q=tmp;
						j=(int)((K_S[i++]<<24)&0xff000000)|((K_S[i++]<<16)&0x00ff0000)|
							((K_S[i++]<<8)&0x0000ff00)|((K_S[i++])&0x000000ff);
						tmp=new byte[j]; Array.Copy(K_S, i, tmp, 0, j); i+=j;
						g=tmp;
						j=(int)((K_S[i++]<<24)&0xff000000)|((K_S[i++]<<16)&0x00ff0000)|
							((K_S[i++]<<8)&0x0000ff00)|((K_S[i++])&0x000000ff);
						tmp=new byte[j]; Array.Copy(K_S, i, tmp, 0, j); i+=j;
						f=tmp;
	
						//	SignatureDSA sig=new SignatureDSA();
						//	sig.init();

						SignatureDSA sig=null;
						try
						{
							Type t=Type.GetType(session.getConfig("signature.dss"));
							sig=(SignatureDSA)(Activator.CreateInstance(t));
							sig.init();
						}
						catch(Exception ee)
						{
							Console.WriteLine(ee);
						}

						sig.setPubKey(f, p, q, g);   
						sig.update(H);
						result=sig.verify(sig_of_H);
					}
					else
					{
						Console.WriteLine("unknow alg");
					}	    
					state=STATE_END;
					break;
			}
			return result;
		}

		public override String getKeyType()
		{
			if(type==DSS) return "DSA";
			return "RSA";
		}

		public override int getState(){return state; }
	}

}

