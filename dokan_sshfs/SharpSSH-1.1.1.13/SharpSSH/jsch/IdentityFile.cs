using System;
using System.IO;

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

	internal class IdentityFile : Identity
	{
		String identity;
		byte[] key;
		byte[] iv;
		private JSch jsch;
		private HASH hash;
		private byte[] encoded_data;

		private Cipher cipher;

		// DSA
		private byte[] P_array;    
		private byte[] Q_array;    
		private byte[] G_array;    
		private byte[] pub_array;    
		private byte[] prv_array;    
		 
		// RSA
		private  byte[] n_array;   // modulus
		private  byte[] e_array;   // public exponent
		private  byte[] d_array;   // private exponent

		private byte[] p_array;
		private byte[] q_array;
		private byte[] dmp1_array;
		private byte[] dmq1_array;
		private byte[] iqmp_array;
		 
		//  private String algname="ssh-dss";
		private String algname="ssh-rsa";

		private const int ERROR=0;
		private const int RSA=1;
		private const int DSS=2;
		internal const int UNKNOWN=3;

		private const int OPENSSH=0;
		private const int FSECURE=1;
		private const int PUTTY=2;

		private int type=ERROR;
		private int keytype=OPENSSH;

		private byte[] publickeyblob=null;

		private bool encrypted=true;

		internal IdentityFile(String identity, JSch jsch) 
		{
			this.identity=identity;
			this.jsch=jsch;
			try
			{
				Type c=Type.GetType(jsch.getConfig("3des-cbc"));
				cipher=(Cipher)Activator.CreateInstance(c);
				key=new byte[cipher.getBlockSize()];   // 24
				iv=new byte[cipher.getIVSize()];       // 8
				c=Type.GetType(jsch.getConfig("md5"));
				hash=(HASH)(Activator.CreateInstance(c));
				hash.init();
				FileInfo file=new FileInfo(identity);
				FileStream fis = File.OpenRead(identity);
				byte[] buf=new byte[(int)(file.Length)];
				int len=fis.Read(buf, 0, buf.Length);
				fis.Close();

				int i=0;
				while(i<len)
				{
					if(buf[i]=='B'&& buf[i+1]=='E'&& buf[i+2]=='G'&& buf[i+3]=='I')
					{
						i+=6;	    
						if(buf[i]=='D'&& buf[i+1]=='S'&& buf[i+2]=='A'){ type=DSS; }
						else if(buf[i]=='R'&& buf[i+1]=='S'&& buf[i+2]=='A'){ type=RSA; }
						else if(buf[i]=='S'&& buf[i+1]=='S'&& buf[i+2]=='H')
						{ // FSecure
							type=UNKNOWN;
							keytype=FSECURE;
						}
						else
						{
							//System.out.println("invalid format: "+identity);
							throw new JSchException("invaid privatekey: "+identity);
						}
						i+=3;
						continue;
					}
					if(buf[i]=='C'&& buf[i+1]=='B'&& buf[i+2]=='C'&& buf[i+3]==',')
					{
						i+=4;
						for(int ii=0; ii<iv.Length; ii++)
						{
							iv[ii]=(byte)(((a2b(buf[i++])<<4)&0xf0)+
								(a2b(buf[i++])&0xf));
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
					throw new JSchException("invaid privatekey: "+identity);
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
				encoded_data=Util.fromBase64(buf, start, i-start);

				if(encoded_data.Length>4 &&            // FSecure
					encoded_data[0]==(byte)0x3f &&
					encoded_data[1]==(byte)0x6f &&
					encoded_data[2]==(byte)0xf9 &&
					encoded_data[3]==(byte)0xeb)
				{

					Buffer _buf=new Buffer(encoded_data);
					_buf.getInt();  // 0x3f6ff9be
					_buf.getInt();
					byte[]_type=_buf.getString();
					//System.out.println("type: "+new String(_type)); 
					byte[] _cipher=_buf.getString();
					String s_cipher=System.Text.Encoding.Default.GetString(_cipher);
					//System.out.println("cipher: "+cipher); 
					if(s_cipher.Equals("3des-cbc"))
					{
						_buf.getInt();
						byte[] foo=new byte[encoded_data.Length-_buf.getOffSet()];
						_buf.getByte(foo);
						encoded_data=foo;
						encrypted=true;
						throw new JSchException("unknown privatekey format: "+identity);
					}
					else if(s_cipher.Equals("none"))
					{
						_buf.getInt();
						//_buf.getInt();

						encrypted=false;

						byte[] foo=new byte[encoded_data.Length-_buf.getOffSet()];
						_buf.getByte(foo);
						encoded_data=foo;
					}

				}

				try
				{
					file=new FileInfo(identity+".pub");
					fis=File.OpenRead(identity+".pub");
					buf=new byte[(int)(file.Length)];
					len=fis.Read(buf, 0, buf.Length);
					fis.Close();
				}
				catch
				{
					return;
				}

				if(buf.Length>4 &&             // FSecure's public key
					buf[0]=='-' && buf[1]=='-' && buf[2]=='-' && buf[3]=='-')
				{

					i=0;
					do{i++;}while(buf.Length>i && buf[i]!=0x0a);
					if(buf.Length<=i) return;

					while(true)
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
					if(buf.Length<=i) return;

					start=i;
					while(i<len)
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
					publickeyblob=Util.fromBase64(buf, start, i-start);

					if(type==UNKNOWN)
					{
						if(publickeyblob[8]=='d')
						{
							type=DSS;
						}
						else if(publickeyblob[8]=='r')
						{
							type=RSA;
						}
					}
				}
				else
				{
					if(buf[0]!='s'|| buf[1]!='s'|| buf[2]!='h'|| buf[3]!='-') return;
					i=0;
					while(i<len){ if(buf[i]==' ')break; i++;} i++;
					if(i>=len) return;
					start=i;
					while(i<len){ if(buf[i]==' ')break; i++;}
					publickeyblob=Util.fromBase64(buf, start, i-start);
				}

			}
			catch(Exception e)
			{
				Console.WriteLine("Identity: "+e);
				if(e is JSchException) throw (JSchException)e;
				throw new JSchException(e.ToString());
			}

		}

		public String getAlgName()
		{
			if(type==RSA) return "ssh-rsa";
			return "ssh-dss"; 
		}

		public bool setPassphrase(String _passphrase) 
		{
			/*
			hash is MD5
			h(0) <- hash(passphrase, iv);
			h(n) <- hash(h(n-1), passphrase, iv);
			key <- (h(0),...,h(n))[0,..,key.Length];
			*/
			try
			{
				if(encrypted)
				{
					if(_passphrase==null) return false;
					byte[] passphrase= System.Text.Encoding.Default.GetBytes( _passphrase );
					int hsize=hash.getBlockSize();
					byte[] hn=new byte[key.Length/hsize*hsize+
						(key.Length%hsize==0?0:hsize)];
					byte[] tmp=null;
					if(keytype==OPENSSH)
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
					else if(keytype==FSECURE)
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
				if(decrypt())
				{
					encrypted=false;
					return true;
				}
				P_array=Q_array=G_array=pub_array=prv_array=null;
				return false;
			}
			catch(Exception e)
			{
				if(e is JSchException) throw (JSchException)e;
				throw new JSchException(e.ToString());
			}
		}

		public byte[] getPublicKeyBlob()
		{
			if(publickeyblob!=null) return publickeyblob;
			if(type==RSA) return getPublicKeyBlob_rsa();
			return getPublicKeyBlob_dss();
		}

		byte[] getPublicKeyBlob_rsa()
		{
			if(e_array==null) return null;
			Buffer buf=new Buffer("ssh-rsa".Length+4+
				e_array.Length+4+ 
				n_array.Length+4);
			buf.putString( System.Text.Encoding.Default.GetBytes( "ssh-rsa" ) );
			buf.putString(e_array);
			buf.putString(n_array);
			return buf.buffer;
		}

		byte[] getPublicKeyBlob_dss()
		{
			if(P_array==null) return null;
			Buffer buf=new Buffer("ssh-dss".Length+4+
				P_array.Length+4+ 
				Q_array.Length+4+ 
				G_array.Length+4+ 
				pub_array.Length+4);
			buf.putString(System.Text.Encoding.Default.GetBytes("ssh-dss"));
			buf.putString(P_array);
			buf.putString(Q_array);
			buf.putString(G_array);
			buf.putString(pub_array);
			return buf.buffer;
		}

		public byte[] getSignature(Session session, byte[] data)
		{
			if(type==RSA) return getSignature_rsa(session, data);
			return getSignature_dss(session, data);
		}

		byte[] getSignature_rsa(Session session, byte[] data)
		{
			try
			{      
				Type t=Type.GetType(jsch.getConfig("signature.rsa"));
				SignatureRSA rsa=(SignatureRSA)Activator.CreateInstance(t);

				rsa.init();
				rsa.setPrvKey(e_array, n_array, d_array, p_array, q_array, dmp1_array, dmq1_array, iqmp_array);

				/*
				byte[] goo=new byte[4];
				goo[0]=(byte)(session.getSessionId().Length>>>24);
				goo[1]=(byte)(session.getSessionId().Length>>>16);
				goo[2]=(byte)(session.getSessionId().Length>>>8);
				goo[3]=(byte)(session.getSessionId().Length);
				rsa.update(goo);
				rsa.update(session.getSessionId());
				*/
				rsa.update(data);
				byte[] sig = rsa.sign();
				Buffer buf=new Buffer("ssh-rsa".Length+4+
					sig.Length+4);
				buf.putString( System.Text.Encoding.Default.GetBytes( "ssh-rsa" ));
				buf.putString(sig);
				return buf.buffer;
			}
			catch(Exception e)
			{
				Console.WriteLine(e);
			}
			return null;
		}

		byte[] getSignature_dss(Session session, byte[] data)
		{
			/*
				byte[] foo;
				int i;
				System.out.print("P ");
				foo=P_array;
				for(i=0;  i<foo.Length; i++){
				System.out.print(Integer.toHexString(foo[i]&0xff)+":");
				}
				System.out.println("");
				System.out.print("Q ");
				foo=Q_array;
				for(i=0;  i<foo.Length; i++){
				System.out.print(Integer.toHexString(foo[i]&0xff)+":");
				}
				System.out.println("");
				System.out.print("G ");
				foo=G_array;
				for(i=0;  i<foo.Length; i++){
				System.out.print(Integer.toHexString(foo[i]&0xff)+":");
				}
				System.out.println("");
			*/

			try
			{      
				Type t=Type.GetType(jsch.getConfig("signature.dss"));
				SignatureDSA dsa=(SignatureDSA)(Activator.CreateInstance(t));
				dsa.init();
				dsa.setPrvKey(prv_array, P_array, Q_array, G_array);

				/*
				byte[] goo=new byte[4];
				goo[0]=(byte)(session.getSessionId().Length>>>24);
				goo[1]=(byte)(session.getSessionId().Length>>>16);
				goo[2]=(byte)(session.getSessionId().Length>>>8);
				goo[3]=(byte)(session.getSessionId().Length);
				dsa.update(goo);
				dsa.update(session.getSessionId());
				*/
				dsa.update(data);
				byte[] sig = dsa.sign();
				Buffer buf=new Buffer("ssh-dss".Length+4+
					sig.Length+4);
				buf.putString( System.Text.Encoding.Default.GetBytes( "ssh-dss" ) );
				buf.putString(sig);
				return buf.buffer;
			}
			catch(Exception e)
			{
				Console.WriteLine("e "+e);
			}
			return null;
		}

		public bool decrypt()
		{
			if(type==RSA) return decrypt_rsa();
			return decrypt_dss();
		}

		bool decrypt_rsa()
		{
//			byte[] p_array;
//			byte[] q_array;
//			byte[] dmp1_array;
//			byte[] dmq1_array;
//			byte[] iqmp_array;

			try
			{
				byte[] plain;
				if(encrypted)
				{
					if(keytype==OPENSSH)
					{
						cipher.init(Cipher.DECRYPT_MODE, key, iv);
						plain=new byte[encoded_data.Length];
						cipher.update(encoded_data, 0, encoded_data.Length, plain, 0);
					}
					else if(keytype==FSECURE)
					{
						for(int i=0; i<iv.Length; i++)iv[i]=0;
						cipher.init(Cipher.DECRYPT_MODE, key, iv);
						plain=new byte[encoded_data.Length];
						cipher.update(encoded_data, 0, encoded_data.Length, plain, 0);
					}
					else
					{
						return false;
					}
				}
				else
				{
					if(n_array!=null) return true;
					plain=encoded_data;
				}

				if(keytype==FSECURE)
				{              // FSecure   
					Buffer buf=new Buffer(plain);
					int foo=buf.getInt();
					if(plain.Length!=foo+4)
					{
						return false;
					}
					e_array=buf.getMPIntBits();
					d_array=buf.getMPIntBits();
					n_array=buf.getMPIntBits();
					byte[] u_array=buf.getMPIntBits();
					p_array=buf.getMPIntBits();
					q_array=buf.getMPIntBits();
					return true;
				}

				int index=0;
				int Length=0;

				if(plain[index]!=0x30)return false;
				index++; // SEQUENCE
				Length=plain[index++]&0xff;
				if((Length&0x80)!=0)
				{
					int foo=Length&0x7f; Length=0;
					while(foo-->0){ Length=(Length<<8)+(plain[index++]&0xff); }
				}

				if(plain[index]!=0x02)return false;
				index++; // INTEGER
				Length=plain[index++]&0xff;
				if((Length&0x80)!=0)
				{
					int foo=Length&0x7f; Length=0;
					while(foo-->0){ Length=(Length<<8)+(plain[index++]&0xff); }
				}
				index+=Length;

				//System.out.println("int: len="+Length);
				//System.out.print(Integer.toHexString(plain[index-1]&0xff)+":");
				//System.out.println("");

				index++;
				Length=plain[index++]&0xff;
				if((Length&0x80)!=0)
				{
					int foo=Length&0x7f; Length=0;
					while(foo-->0){ Length=(Length<<8)+(plain[index++]&0xff); }
				}
				n_array=new byte[Length];
				Array.Copy(plain, index, n_array, 0, Length);
				index+=Length;
				/*
				System.out.println("int: N len="+Length);
				for(int i=0; i<n_array.Length; i++){
				System.out.print(Integer.toHexString(n_array[i]&0xff)+":");
				}
				System.out.println("");
				*/
				index++;
				Length=plain[index++]&0xff;
				if((Length&0x80)!=0)
				{
					int foo=Length&0x7f; Length=0;
					while(foo-->0){ Length=(Length<<8)+(plain[index++]&0xff); }
				}
				e_array=new byte[Length];
				Array.Copy(plain, index, e_array, 0, Length);
				index+=Length;
				/*
				System.out.println("int: E len="+Length);
				for(int i=0; i<e_array.Length; i++){
				System.out.print(Integer.toHexString(e_array[i]&0xff)+":");
				}
				System.out.println("");
				*/
				index++;
				Length=plain[index++]&0xff;
				if((Length&0x80)!=0)
				{
					int foo=Length&0x7f; Length=0;
					while(foo-->0){ Length=(Length<<8)+(plain[index++]&0xff); }
				}
				d_array=new byte[Length];
				Array.Copy(plain, index, d_array, 0, Length);
				index+=Length;
				/*
				System.out.println("int: D len="+Length);
				for(int i=0; i<d_array.Length; i++){
				System.out.print(Integer.toHexString(d_array[i]&0xff)+":");
				}
				System.out.println("");
				*/

				index++;
				Length=plain[index++]&0xff;
				if((Length&0x80)!=0)
				{
					int foo=Length&0x7f; Length=0;
					while(foo-->0){ Length=(Length<<8)+(plain[index++]&0xff); }
				}
				p_array=new byte[Length];
				Array.Copy(plain, index, p_array, 0, Length);
				index+=Length;
				/*
				System.out.println("int: P len="+Length);
				for(int i=0; i<p_array.Length; i++){
				System.out.print(Integer.toHexString(p_array[i]&0xff)+":");
				}
				System.out.println("");
				*/
				index++;
				Length=plain[index++]&0xff;
				if((Length&0x80)!=0)
				{
					int foo=Length&0x7f; Length=0;
					while(foo-->0){ Length=(Length<<8)+(plain[index++]&0xff); }
				}
				q_array=new byte[Length];
				Array.Copy(plain, index, q_array, 0, Length);
				index+=Length;
				/*
				System.out.println("int: q len="+Length);
				for(int i=0; i<q_array.Length; i++){
				System.out.print(Integer.toHexString(q_array[i]&0xff)+":");
				}
				System.out.println("");
				*/
				index++;
				Length=plain[index++]&0xff;
				if((Length&0x80)!=0)
				{
					int foo=Length&0x7f; Length=0;
					while(foo-->0){ Length=(Length<<8)+(plain[index++]&0xff); }
				}
				dmp1_array=new byte[Length];
				Array.Copy(plain, index, dmp1_array, 0, Length);
				index+=Length;
				/*
				System.out.println("int: dmp1 len="+Length);
				for(int i=0; i<dmp1_array.Length; i++){
				System.out.print(Integer.toHexString(dmp1_array[i]&0xff)+":");
				}
				System.out.println("");
				*/
				index++;
				Length=plain[index++]&0xff;
				if((Length&0x80)!=0)
				{
					int foo=Length&0x7f; Length=0;
					while(foo-->0){ Length=(Length<<8)+(plain[index++]&0xff); }
				}
				dmq1_array=new byte[Length];
				Array.Copy(plain, index, dmq1_array, 0, Length);
				index+=Length;
				/*
				System.out.println("int: dmq1 len="+Length);
				for(int i=0; i<dmq1_array.Length; i++){
				System.out.print(Integer.toHexString(dmq1_array[i]&0xff)+":");
				}
				System.out.println("");
				*/
				index++;
				Length=plain[index++]&0xff;
				if((Length&0x80)!=0)
				{
					int foo=Length&0x7f; Length=0;
					while(foo-->0){ Length=(Length<<8)+(plain[index++]&0xff); }
				}
				iqmp_array=new byte[Length];
				Array.Copy(plain, index, iqmp_array, 0, Length);
				index+=Length;
				/*
				System.out.println("int: iqmp len="+Length);
				for(int i=0; i<iqmp_array.Length; i++){
				System.out.print(Integer.toHexString(iqmp_array[i]&0xff)+":");
				}
				System.out.println("");
				*/
			}
			catch
			{
				//System.out.println(e);
				return false;
			}
			return true;
		}

		bool decrypt_dss()
		{
			try
			{
				byte[] plain;
				if(encrypted)
				{
					if(keytype==OPENSSH)
					{
						cipher.init(Cipher.DECRYPT_MODE, key, iv);
						plain=new byte[encoded_data.Length];
						cipher.update(encoded_data, 0, encoded_data.Length, plain, 0);
						/*
						for(int i=0; i<plain.Length; i++){
						System.out.print(Integer.toHexString(plain[i]&0xff)+":");
						}
						System.out.println("");
						*/
					}
					else if(keytype==FSECURE)
					{
						for(int i=0; i<iv.Length; i++)iv[i]=0;
						cipher.init(Cipher.DECRYPT_MODE, key, iv);
						plain=new byte[encoded_data.Length];
						cipher.update(encoded_data, 0, encoded_data.Length, plain, 0);
					}
					else
					{
						return false;
					}
				}
				else
				{
					if(P_array!=null) return true;
					plain=encoded_data;
				}

				if(keytype==FSECURE)
				{              // FSecure   
					Buffer buf=new Buffer(plain);
					int foo=buf.getInt();
					if(plain.Length!=foo+4)
					{
						return false;
					}
					P_array=buf.getMPIntBits();
					G_array=buf.getMPIntBits();
					Q_array=buf.getMPIntBits();
					pub_array=buf.getMPIntBits();
					prv_array=buf.getMPIntBits();
					return true;
				}

				int index=0;
				int Length=0;

				if(plain[index]!=0x30)return false;
				index++; // SEQUENCE
				Length=plain[index++]&0xff;
				if((Length&0x80)!=0)
				{
					int foo=Length&0x7f; Length=0;
					while(foo-->0){ Length=(Length<<8)+(plain[index++]&0xff); }
				}

				if(plain[index]!=0x02)return false;
				index++; // INTEGER
				Length=plain[index++]&0xff;
				if((Length&0x80)!=0)
				{
					int foo=Length&0x7f; Length=0;
					while(foo-->0){ Length=(Length<<8)+(plain[index++]&0xff); }
				}
				index+=Length;

				index++;
				Length=plain[index++]&0xff;
				if((Length&0x80)!=0)
				{
					int foo=Length&0x7f; Length=0;
					while(foo-->0){ Length=(Length<<8)+(plain[index++]&0xff); }
				}
				P_array=new byte[Length];
				Array.Copy(plain, index, P_array, 0, Length);
				index+=Length;

				index++;
				Length=plain[index++]&0xff;
				if((Length&0x80)!=0)
				{
					int foo=Length&0x7f; Length=0;
					while(foo-->0){ Length=(Length<<8)+(plain[index++]&0xff); }
				}
				Q_array=new byte[Length];
				Array.Copy(plain, index, Q_array, 0, Length);
				index+=Length;

				index++;
				Length=plain[index++]&0xff;
				if((Length&0x80)!=0)
				{
					int foo=Length&0x7f; Length=0;
					while(foo-->0){ Length=(Length<<8)+(plain[index++]&0xff); }
				}
				G_array=new byte[Length];
				Array.Copy(plain, index, G_array, 0, Length);
				index+=Length;

				index++;
				Length=plain[index++]&0xff;
				if((Length&0x80)!=0)
				{
					int foo=Length&0x7f; Length=0;
					while(foo-->0){ Length=(Length<<8)+(plain[index++]&0xff); }
				}
				pub_array=new byte[Length];
				Array.Copy(plain, index, pub_array, 0, Length);
				index+=Length;

				index++;
				Length=plain[index++]&0xff;
				if((Length&0x80)!=0)
				{
					int foo=Length&0x7f; Length=0;
					while(foo-->0){ Length=(Length<<8)+(plain[index++]&0xff); }
				}
				prv_array=new byte[Length];
				Array.Copy(plain, index, prv_array, 0, Length);
				index+=Length;
			}
			catch
			{
				//System.out.println(e);
				//e.printStackTrace();
				return false;
			}
			return true;
		}

		public bool isEncrypted()
		{
			return encrypted;
		}
		public String getName(){return identity;}

		private int writeSEQUENCE(byte[] buf, int index, int len)
		{
			buf[index++]=0x30;
			index=writeLength(buf, index, len);
			return index;
		}
		private int writeINTEGER(byte[] buf, int index, byte[] data)
		{
			buf[index++]=0x02;
			index=writeLength(buf, index, data.Length);
			Array.Copy(data, 0, buf, index, data.Length);
			index+=data.Length;
			return index;
		}

		private int countLength(int i_len)
		{
			uint len = (uint)i_len;
			int i=1;
			if(len<=0x7f) return i;
			while(len>0)
			{
				len>>=8;
				i++;
			}
			return i;
		}

		private int writeLength(byte[] data, int index, int i_len)
		{
			int len = (int)i_len;
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

		private byte a2b(byte c)
		{
			if('0'<=c&&c<='9') return (byte)(c-'0');
			if('a'<=c&&c<='z') return (byte)(c-'a'+10);
			return (byte)(c-'A'+10);
		}
		private byte b2a(byte c)
		{
			if(0<=c&&c<=9) return (byte)(c+'0');
			return (byte)(c-10+'A');
		}
	}

}
