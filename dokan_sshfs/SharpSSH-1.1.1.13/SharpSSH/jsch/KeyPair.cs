using System;
using System.IO;
using System.Runtime.CompilerServices;


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

	public abstract class KeyPair
	{
		public const int ERROR=0;
		public const int DSA=1;
		public const int RSA=2;
		public const int UNKNOWN=3;

		internal const int VENDOR_OPENSSH=0;
		internal const int VENDOR_FSECURE=1;
		internal int vendor=VENDOR_OPENSSH;

		private static byte[] cr=Util.getBytes("\n");

		public static KeyPair genKeyPair(JSch jsch, int type)
		{
			return genKeyPair(jsch, type, 1024);
		}
		public static KeyPair genKeyPair(JSch jsch, int type, int key_size)
		{
			KeyPair kpair=null;
			if(type==DSA){ kpair=new KeyPairDSA(jsch); }
			else if(type==RSA){ kpair=new KeyPairRSA(jsch); }
			if(kpair!=null)
			{
				kpair.generate(key_size);
			}
			return kpair;
		}

		internal abstract void generate(int key_size);

		internal abstract byte[] getBegin();
		internal abstract byte[] getEnd();
		public abstract int getKeySize();

		internal JSch jsch=null;
		private Cipher cipher;
		private HASH hash;
		private Random random;

		private byte[] passphrase;

		public KeyPair(JSch jsch)
		{
			this.jsch=jsch;
		}

		static byte[][] header={Util.getBytes( "Proc-Type: 4,ENCRYPTED"),
								   Util.getBytes("DEK-Info: DES-EDE3-CBC,")};

		internal abstract byte[] getPrivateKey();

		private void Write(Stream s, byte[] arr)
		{
			s.Write(arr, 0, arr.Length);
		}

		public void writePrivateKey(Stream outs)
		{
			byte[] plain=getPrivateKey();
			byte[][] _iv=new byte[1][];
			byte[] encoded=encrypt(plain, _iv);
			byte[] iv=_iv[0];
			byte[] prv=Util.toBase64(encoded, 0, encoded.Length);

			try
			{
				Write(outs, getBegin()); Write(outs,cr);
				if(passphrase!=null)
				{
					Write(outs, header[0]); Write(outs,cr);
					Write(outs, header[1]); 
					for(int j=0; j<iv.Length; j++)
					{
						outs.WriteByte(b2a((byte)((iv[j]>>4)&0x0f)));
						outs.WriteByte(b2a((byte)(iv[j]&0x0f)));
					}
					Write(outs,cr);
					Write(outs,cr);
				}
				int i=0;
				while(i<prv.Length)
				{
					if(i+64<prv.Length)
					{
						outs.Write(prv, i, 64);
						Write(outs,cr);
						i+=64;
						continue;
					}
					outs.Write(prv, i, prv.Length-i);
					Write(outs,cr);
					break;
				}
				Write(outs, getEnd()); Write(outs,cr);
				//outs.close();
			}
			catch(Exception e)
			{
				Console.WriteLine(e);
			}
		}

		private static byte[] space=Util.getBytes(" ");

		internal abstract byte[] getKeyTypeName();
		public abstract int getKeyType();

		public virtual byte[] getPublicKeyBlob(){ return publickeyblob; }

		public void writePublicKey(Stream outs, String comment)
		{
			byte[] pubblob=getPublicKeyBlob();
			byte[] pub=Util.toBase64(pubblob, 0, pubblob.Length);
			try
			{
				Write(outs, getKeyTypeName()); Write(outs, space);
				outs.Write(pub, 0, pub.Length); Write(outs, space);
				Write(outs, Util.getBytes( comment));
				Write(outs,cr);
			}
			catch(Exception e)
			{
				Console.WriteLine(e);
			}
		}

		public void writePublicKey(String name, String comment) 
		{
			FileStream fos=new FileStream(name,FileMode.OpenOrCreate);
			writePublicKey(fos, comment);
			fos.Close();
		}

		public void writeSECSHPublicKey(Stream outs, String comment)
		{
			byte[] pubblob=getPublicKeyBlob();
			byte[] pub=Util.toBase64(pubblob, 0, pubblob.Length);
			try
			{
				Write(outs, Util.getBytes( "---- BEGIN SSH2 PUBLIC KEY ----")); Write(outs, cr);
				Write(outs, Util.getBytes("Comment: \""+comment+"\"")); Write(outs,cr);
				int index=0;
				while(index<pub.Length)
				{
					int len=70;
					if((pub.Length-index)<len)len=pub.Length-index;
					outs.Write(pub, index, len); Write(outs, cr);
					index+=len;
				}
				Write(outs, Util.getBytes("---- END SSH2 PUBLIC KEY ----")); Write(outs,cr);
			}
			catch(Exception e)
			{
				Console.WriteLine(e);
			}
		}

		public void writeSECSHPublicKey(String name, String comment)
		{
			FileStream fos=new FileStream(name, FileMode.OpenOrCreate);
			writeSECSHPublicKey(fos, comment);
			fos.Close();
		}


		public void writePrivateKey(String name) 
		{
			FileStream fos=new FileStream(name, FileMode.OpenOrCreate);
			writePrivateKey(fos);
			fos.Close();
		}

		public String getFingerPrint()
		{
			if(hash==null) hash=genHash();
			byte[] kblob=getPublicKeyBlob();
			if(kblob==null) return null;
			return getKeySize()+" "+Util.getFingerPrint(hash, kblob);
		}

		private byte[] encrypt(byte[] plain, byte[][] _iv)
		{
			if(passphrase==null) return plain;

			if(cipher==null) cipher=genCipher();
			byte[] iv=_iv[0]=new byte[cipher.getIVSize()];

			if(random==null) random=genRandom();
			random.fill(iv, 0, iv.Length);

			byte[] key=genKey(passphrase, iv);
			byte[] encoded=plain;
			int bsize=cipher.getBlockSize();
			if(encoded.Length%bsize!=0)
			{
				byte[] foo=new byte[(encoded.Length/bsize+1)*bsize];
				Array.Copy(encoded, 0, foo, 0, encoded.Length);
				encoded=foo;
			}

			try
			{
				cipher.init(Cipher.ENCRYPT_MODE, key, iv);
				cipher.update(encoded, 0, encoded.Length, encoded, 0);
			}
			catch(Exception e)
			{
				Console.WriteLine(e);
			}
			return encoded;
		}

		internal abstract bool parse(byte[] data);

		private byte[] decrypt(byte[] data, byte[] passphrase, byte[] iv)
		{
			/*
			if(iv==null){  // FSecure
			  iv=new byte[8];
			  for(int i=0; i<iv.Length; i++)iv[i]=0;
			}
			*/
			try
			{
				byte[] key=genKey(passphrase, iv);
				cipher.init(Cipher.DECRYPT_MODE, key, iv);
				byte[] plain=new byte[data.Length];
				cipher.update(data, 0, data.Length, plain, 0);
				return plain;
			}
			catch(Exception e)
			{
				Console.WriteLine(e);
			}
			return null;
		}

		internal int writeSEQUENCE(byte[] buf, int index, int len)
		{
			buf[index++]=0x30;
			index=writeLength(buf, index, len);
			return index;
		}
		internal int writeINTEGER(byte[] buf, int index, byte[] data)
		{
			buf[index++]=0x02;
			index=writeLength(buf, index, data.Length);
			Array.Copy(data, 0, buf, index, data.Length);
			index+=data.Length;
			return index;
		}

		internal int countLength(int len)
		{
			int i=1;
			if(len<=0x7f) return i;
			while(len>0)
			{
				len>>=8;
				i++;
			}
			return i;
		}

		internal int writeLength(byte[] data, int index, int len)
		{
			int i=countLength(len)-1;
			if(i==0)
			{
				data[index++]=(byte)len;
				return index;
			}
			data[index++]=(byte)(0x80|i);
			int j=index+i;
			while(i>0)
			{
				data[index+i-1]=(byte)(len&0xff);
				len>>=8;
				i--;
			}
			return j;
		}

		private Random genRandom()
		{
			if(random==null)
			{
				try
				{
					Type t=Type.GetType(jsch.getConfig("random"));
					random=(Random)Activator.CreateInstance(t);
				}
				catch(Exception e){ Console.Error.WriteLine("connect: random "+e); }
			}
			return random;
		}

		private HASH genHash()
		{
			try
			{
				Type t=Type.GetType(jsch.getConfig("md5"));
				hash=(HASH)Activator.CreateInstance(t);
				hash.init();
			}
			catch//(Exception e)
			{
			}
			return hash;
		}
		private Cipher genCipher()
		{
			try
			{
				Type t;
				t=Type.GetType(jsch.getConfig("3des-cbc"));
				cipher=(Cipher)(Activator.CreateInstance(t));
			}
			catch//(Exception e)
			{
			}
			return cipher;
		}

		/*
		  hash is MD5
		  h(0) <- hash(passphrase, iv);
		  h(n) <- hash(h(n-1), passphrase, iv);
		  key <- (h(0),...,h(n))[0,..,key.Length];
		*/
		[MethodImpl(MethodImplOptions.Synchronized)]
		internal byte[] genKey(byte[] passphrase, byte[] iv)
		{
			if(cipher==null) cipher=genCipher();
			if(hash==null) hash=genHash();

			byte[] key=new byte[cipher.getBlockSize()];
			int hsize=hash.getBlockSize();
			byte[] hn=new byte[key.Length/hsize*hsize+
				(key.Length%hsize==0?0:hsize)];
			try
			{
				byte[] tmp=null;
				if(vendor==VENDOR_OPENSSH)
				{
					for(int index=0; index+hsize<=hn.Length;)
					{
						if(tmp!=null){ hash.update(tmp, 0, tmp.Length); }
						hash.update(passphrase, 0, passphrase.Length);
						hash.update(iv, 0, iv.Length);
						tmp=hash.digest();
						Array.Copy(tmp, 0, hn, index, tmp.Length);
						index+=tmp.Length;
					}
					Array.Copy(hn, 0, key, 0, key.Length); 
				}
				else if(vendor==VENDOR_FSECURE)
				{
					for(int index=0; index+hsize<=hn.Length;)
					{
						if(tmp!=null){ hash.update(tmp, 0, tmp.Length); }
						hash.update(passphrase, 0, passphrase.Length);
						tmp=hash.digest();
						Array.Copy(tmp, 0, hn, index, tmp.Length);
						index+=tmp.Length;
					}
					Array.Copy(hn, 0, key, 0, key.Length); 
				}
			}
			catch(Exception e)
			{
				Console.WriteLine(e);
			}
			return key;
		} 

		public void setPassphrase(String passphrase)
		{
			if(passphrase==null || passphrase.Length==0)
			{
				setPassphrase((byte[])null);
			}
			else
			{
				setPassphrase(Util.getBytes( passphrase ));
			}
		}
		public void setPassphrase(byte[] passphrase)
		{
			if(passphrase!=null && passphrase.Length==0) 
				passphrase=null;
			this.passphrase=passphrase;
		}

		private bool encrypted=false;
		private byte[] data=null;
		private byte[] iv=null;
		private byte[] publickeyblob=null;

		public bool isEncrypted(){ return encrypted; }
		public bool decrypt(String _passphrase)
		{
			byte[] passphrase= Util.getBytes( _passphrase );
			byte[] foo=decrypt(data, passphrase, iv);
			if(parse(foo))
			{
				encrypted=false;
			}
			return !encrypted;
		}

		public static KeyPair load(JSch jsch, String prvkey)
		{
			String pubkey=prvkey+".pub";
//			if(!new File(pubkey).exists())
			if(!File.Exists(pubkey))
			{
				pubkey=null;
			}
			return load(jsch, prvkey, pubkey);
		}
		public static KeyPair load(JSch jsch, String prvkey, String pubkey)
		{

			byte[] iv=new byte[8];       // 8
			bool encrypted=true;
			byte[] data=null;

			byte[] publickeyblob=null;

			int type=ERROR;
			int vendor=VENDOR_OPENSSH;

			try
			{
				//File file=new File(prvkey);
				FileStream fis=File.OpenRead(prvkey);
				byte[] buf=new byte[(int)(fis.Length)];
				int len=fis.Read(buf, 0, buf.Length);
				fis.Close();

				int i=0;

				while(i<len)
				{
					if(buf[i]=='B'&& buf[i+1]=='E'&& buf[i+2]=='G'&& buf[i+3]=='I')
					{
						i+=6;	    
						if(buf[i]=='D'&& buf[i+1]=='S'&& buf[i+2]=='A'){ type=DSA; }
						else if(buf[i]=='R'&& buf[i+1]=='S'&& buf[i+2]=='A'){ type=RSA; }
						else if(buf[i]=='S'&& buf[i+1]=='S'&& buf[i+2]=='H')
						{ // FSecure
							type=UNKNOWN;
							vendor=VENDOR_FSECURE;
						}
						else
						{
							//System.outs.println("invalid format: "+identity);
							throw new JSchException("invaid privatekey: "+prvkey);
						}
						i+=3;
						continue;
					}
					if(buf[i]=='C'&& buf[i+1]=='B'&& buf[i+2]=='C'&& buf[i+3]==',')
					{
						i+=4;
						for(int ii=0; ii<iv.Length; ii++)
						{
							iv[ii]=(byte)(((a2b(buf[i++])<<4)&0xf0)+(a2b(buf[i++])&0xf));
						}
						continue;
					}
					if(buf[i]==0x0d &&
						i+1<buf.Length && buf[i+1]==0x0a)
					{
						i++;
						continue;
					}
					if(buf[i]==0x0a && i+1<buf.Length)
					{
						if(buf[i+1]==0x0a){ i+=2; break; }
						if(buf[i+1]==0x0d &&
							i+2<buf.Length && buf[i+2]==0x0a)
						{
							i+=3; break;
						}
						bool inheader=false;
						for(int j=i+1; j<buf.Length; j++)
						{
							if(buf[j]==0x0a) break;
							//if(buf[j]==0x0d) break;
							if(buf[j]==':'){inheader=true; break;}
						}
						if(!inheader)
						{
							i++; 
							encrypted=false;    // no passphrase
							break;
						}
					}
					i++;
				}

				if(type==ERROR)
				{
					throw new JSchException("invaid privatekey: "+prvkey);
				}

				int start=i;
				while(i<len)
				{
					if(buf[i]==0x0a)
					{
						bool xd=(buf[i-1]==0x0d);
						Array.Copy(buf, i+1, 
							buf, 
							i-(xd ? 1 : 0), 
							len-i-1-(xd ? 1 : 0)
							);
						if(xd)len--;
						len--;
						continue;
					}
					if(buf[i]=='-'){  break; }
					i++;
				}
				data=Util.fromBase64(buf, start, i-start);

				if(data.Length>4 &&            // FSecure
					data[0]==(byte)0x3f &&
					data[1]==(byte)0x6f &&
					data[2]==(byte)0xf9 &&
					data[3]==(byte)0xeb)
				{

					Buffer _buf=new Buffer(data);
					_buf.getInt();  // 0x3f6ff9be
					_buf.getInt();
					byte[]_type=_buf.getString();
					//System.outs.println("type: "+new String(_type)); 
					byte[] _cipher=_buf.getString();
					String cipher=Util.getString(_cipher);
					//System.outs.println("cipher: "+cipher); 
					if(cipher.Equals("3des-cbc"))
					{
						_buf.getInt();
						byte[] foo=new byte[data.Length-_buf.getOffSet()];
						_buf.getByte(foo);
						data=foo;
						encrypted=true;
						throw new JSchException("unknown privatekey format: "+prvkey);
					}
					else if(cipher.Equals("none"))
					{
						_buf.getInt();
						_buf.getInt();

						encrypted=false;

						byte[] foo=new byte[data.Length-_buf.getOffSet()];
						_buf.getByte(foo);
						data=foo;
					}
				}

				if(pubkey!=null)
				{
					try
					{
						//file=new File(pubkey);
						fis=File.OpenRead(pubkey);
						buf=new byte[(int)(fis.Length)];
						len=fis.Read(buf, 0, buf.Length);
						fis.Close();

						if(buf.Length>4 &&             // FSecure's public key
							buf[0]=='-' && buf[1]=='-' && buf[2]=='-' && buf[3]=='-')
						{

							bool valid=true;
							i=0;
							do{i++;}while(buf.Length>i && buf[i]!=0x0a);
							if(buf.Length<=i) {valid=false;}

							while(valid)
							{
								if(buf[i]==0x0a)
								{
									bool inheader=false;
									for(int j=i+1; j<buf.Length; j++)
									{
										if(buf[j]==0x0a) break;
										if(buf[j]==':'){inheader=true; break;}
									}
									if(!inheader)
									{
										i++; 
										break;
									}
								}
								i++;
							}
							if(buf.Length<=i){valid=false;}

							start=i;
							while(valid && i<len)
							{
								if(buf[i]==0x0a)
								{
									Array.Copy(buf, i+1, buf, i, len-i-1);
									len--;
									continue;
								}
								if(buf[i]=='-'){  break; }
								i++;
							}
							if(valid)
							{
								publickeyblob=Util.fromBase64(buf, start, i-start);
								if(type==UNKNOWN)
								{
									if(publickeyblob[8]=='d'){ type=DSA; }
									else if(publickeyblob[8]=='r'){ type=RSA; }
								}
							}
						}
						else
						{
							if(buf[0]=='s'&& buf[1]=='s'&& buf[2]=='h' && buf[3]=='-')
							{
								i=0;
								while(i<len){ if(buf[i]==' ')break; i++;} i++;
								if(i<len)
								{
									start=i;
									while(i<len){ if(buf[i]==' ')break; i++;}
									publickeyblob=Util.fromBase64(buf, start, i-start);
								}
							}
						}
					}
					catch//(Exception ee)
					{
					}
				}
			}
			catch(Exception e)
			{
				if(e is JSchException) throw (JSchException)e;
				throw new JSchException(e.ToString());
			}

			KeyPair kpair=null;
			if(type==DSA){ kpair=new KeyPairDSA(jsch); }
			else if(type==RSA){ kpair=new KeyPairRSA(jsch); }

			if(kpair!=null)
			{
				kpair.encrypted=encrypted;
				kpair.publickeyblob=publickeyblob;
				kpair.vendor=vendor;

				if(encrypted)
				{
					kpair.iv=iv;
					kpair.data=data;
				}
				else
				{
					if(kpair.parse(data))
					{
						return kpair;
					}
					else
					{
						throw new JSchException("invaid privatekey: "+prvkey);
					}
				}
			}

			return kpair;
		}

		static private byte a2b(byte c)
		{
			if('0'<=c&&c<='9') return (byte)(c-'0');
			return (byte)(c-'a'+10);
		}
		static private byte b2a(byte c)
		{
			if(0<=c&&c<=9) return (byte)(c+'0');
			return (byte)(c-10+'A');
		}

		public virtual void dispose()
		{
			passphrase=null;
		}
	}

}
