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

	public class KeyPairDSA : KeyPair
	{
		private byte[] P_array;
		private byte[] Q_array;
		private byte[] G_array;
		private byte[] pub_array;
		private byte[] prv_array;

		//private int key_size=0;
		private int key_size=1024;

		public KeyPairDSA(JSch jsch):base(jsch)
		{
		}

		internal override void generate(int key_size) 
		{
			this.key_size=key_size;
			try
			{
				Type t=Type.GetType(jsch.getConfig("keypairgen.dsa"));
				KeyPairGenDSA keypairgen=(KeyPairGenDSA)(Activator.CreateInstance(t));
				keypairgen.init(key_size);
				P_array=keypairgen.getP();
				Q_array=keypairgen.getQ();
				G_array=keypairgen.getG();
				pub_array=keypairgen.getY();
				prv_array=keypairgen.getX();

				keypairgen=null;
			}
			catch(Exception e)
			{
				Console.Error.WriteLine("KeyPairDSA: "+e); 
				throw new JSchException(e.ToString());
			}
		}

		private static byte[] begin= Util.getBytes( "-----BEGIN DSA PRIVATE KEY-----" );
		private static byte[] end=Util.getBytes("-----END DSA PRIVATE KEY-----");

		internal override byte[] getBegin(){ return begin; }
		internal override byte[] getEnd(){ return end; }

		internal override byte[] getPrivateKey()
		{
			int content=
				1+countLength(1) + 1 +                           // INTEGER
				1+countLength(P_array.Length) + P_array.Length + // INTEGER  P
				1+countLength(Q_array.Length) + Q_array.Length + // INTEGER  Q
				1+countLength(G_array.Length) + G_array.Length + // INTEGER  G
				1+countLength(pub_array.Length) + pub_array.Length + // INTEGER  pub
				1+countLength(prv_array.Length) + prv_array.Length;  // INTEGER  prv

			int total=
				1+countLength(content)+content;   // SEQUENCE

			byte[] plain=new byte[total];
			int index=0;
			index=writeSEQUENCE(plain, index, content);
			index=writeINTEGER(plain, index, new byte[1]);  // 0
			index=writeINTEGER(plain, index, P_array);
			index=writeINTEGER(plain, index, Q_array);
			index=writeINTEGER(plain, index, G_array);
			index=writeINTEGER(plain, index, pub_array);
			index=writeINTEGER(plain, index, prv_array);
			return plain;
		}

		internal override bool parse(byte[] plain)
		{
			try
			{

				if(vendor==VENDOR_FSECURE)
				{
					if(plain[0]!=0x30)
					{              // FSecure
						Buffer buf=new Buffer(plain);
						buf.getInt();
						P_array=buf.getMPIntBits();
						G_array=buf.getMPIntBits();
						Q_array=buf.getMPIntBits();
						pub_array=buf.getMPIntBits();
						prv_array=buf.getMPIntBits();
						return true;
					}
					return false;
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
			catch//(Exception e)
			{
				//System.out.println(e);
				//e.printStackTrace();
				return false;
			}
			return true;
		}

		public override byte[] getPublicKeyBlob()
		{
			byte[] foo=base.getPublicKeyBlob();
			if(foo!=null) return foo;

			if(P_array==null) return null;

			Buffer buf=new Buffer(sshdss.Length+4+
				P_array.Length+4+ 
				Q_array.Length+4+ 
				G_array.Length+4+ 
				pub_array.Length+4);
			buf.putString(sshdss);
			buf.putString(P_array);
			buf.putString(Q_array);
			buf.putString(G_array);
			buf.putString(pub_array);
			return buf.buffer;
		}

		private static byte[] sshdss= Util.getBytes( "ssh-dss" );
		internal override byte[] getKeyTypeName(){return sshdss;}
		public override int getKeyType(){return DSA;}

		public override int getKeySize(){return key_size; }
		public override void dispose()
		{
			base.dispose();
			P_array=null;
			Q_array=null;
			G_array=null;
			pub_array=null;
			prv_array=null;
		}
	}

}
