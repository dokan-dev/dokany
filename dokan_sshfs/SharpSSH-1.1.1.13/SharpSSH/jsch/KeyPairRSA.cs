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

	public class KeyPairRSA : KeyPair
	{
		private byte[] prv_array;
		private byte[] pub_array;
		private byte[] n_array;

		private byte[] p_array;  // prime p
		private byte[] q_array;  // prime q
		private byte[] ep_array; // prime exponent p
		private byte[] eq_array; // prime exponent q
		private byte[] c_array;  // coefficient

		//private int key_size=0;
		private int key_size=1024;

		public KeyPairRSA(JSch jsch):base(jsch)
		{
		}

		internal override void generate(int key_size)
		{
			this.key_size=key_size;
			try
			{
				Type t=Type.GetType(jsch.getConfig("keypairgen.rsa"));
				KeyPairGenRSA keypairgen=(KeyPairGenRSA)(Activator.CreateInstance(t));
				keypairgen.init(key_size);
				pub_array=keypairgen.getE();
				prv_array=keypairgen.getD();
				n_array=keypairgen.getN();

				p_array=keypairgen.getP();
				q_array=keypairgen.getQ();
				ep_array=keypairgen.getEP();
				eq_array=keypairgen.getEQ();
				c_array=keypairgen.getC();

				keypairgen=null;
			}
			catch(Exception e)
			{
				Console.WriteLine("KeyPairRSA: "+e); 
				throw new JSchException(e.ToString());
			}
		}

		private static  byte[] begin= Util.getBytes( "-----BEGIN RSA PRIVATE KEY-----");
		private static  byte[] end=Util.getBytes("-----END RSA PRIVATE KEY-----");

		internal override byte[] getBegin(){ return begin; }
		internal override byte[] getEnd(){ return end; }

		internal override byte[] getPrivateKey()
		{
			int content=
				1+countLength(1) + 1 +                           // INTEGER
				1+countLength(n_array.Length) + n_array.Length + // INTEGER  N
				1+countLength(pub_array.Length) + pub_array.Length + // INTEGER  pub
				1+countLength(prv_array.Length) + prv_array.Length+  // INTEGER  prv
				1+countLength(p_array.Length) + p_array.Length+      // INTEGER  p
				1+countLength(q_array.Length) + q_array.Length+      // INTEGER  q
				1+countLength(ep_array.Length) + ep_array.Length+    // INTEGER  ep
				1+countLength(eq_array.Length) + eq_array.Length+    // INTEGER  eq
				1+countLength(c_array.Length) + c_array.Length;      // INTEGER  c

			int total=
				1+countLength(content)+content;   // SEQUENCE

			byte[] plain=new byte[total];
			int index=0;
			index=writeSEQUENCE(plain, index, content);
			index=writeINTEGER(plain, index, new byte[1]);  // 0
			index=writeINTEGER(plain, index, n_array);
			index=writeINTEGER(plain, index, pub_array);
			index=writeINTEGER(plain, index, prv_array);
			index=writeINTEGER(plain, index, p_array);
			index=writeINTEGER(plain, index, q_array);
			index=writeINTEGER(plain, index, ep_array);
			index=writeINTEGER(plain, index, eq_array);
			index=writeINTEGER(plain, index, c_array);
			return plain;
		}

		internal override bool parse(byte [] plain)
		{
			/*
			byte[] p_array;
			byte[] q_array;
			byte[] dmp1_array;
			byte[] dmq1_array;
			byte[] iqmp_array;
			*/
			try
			{
				int index=0;
				int Length=0;

				if(vendor==VENDOR_FSECURE)
				{
					if(plain[index]!=0x30)
					{                  // FSecure
						Buffer buf=new Buffer(plain);
						pub_array=buf.getMPIntBits();
						prv_array=buf.getMPIntBits();
						n_array=buf.getMPIntBits();
						byte[] u_array=buf.getMPIntBits();
						p_array=buf.getMPIntBits();
						q_array=buf.getMPIntBits();
						return true;
					}
					return false;
				}

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
				pub_array=new byte[Length];
				Array.Copy(plain, index, pub_array, 0, Length);
				index+=Length;
				/*
				System.out.println("int: E len="+Length);
				for(int i=0; i<pub_array.Length; i++){
				System.out.print(Integer.toHexString(pub_array[i]&0xff)+":");
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
				prv_array=new byte[Length];
				Array.Copy(plain, index, prv_array, 0, Length);
				index+=Length;
				/*
				System.out.println("int: prv len="+Length);
				for(int i=0; i<prv_array.Length; i++){
				System.out.print(Integer.toHexString(prv_array[i]&0xff)+":");
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
				ep_array=new byte[Length];
				Array.Copy(plain, index, ep_array, 0, Length);
				index+=Length;
				/*
				System.out.println("int: ep len="+Length);
				for(int i=0; i<ep_array.Length; i++){
				System.out.print(Integer.toHexString(ep_array[i]&0xff)+":");
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
				eq_array=new byte[Length];
				Array.Copy(plain, index, eq_array, 0, Length);
				index+=Length;
				/*
				System.out.println("int: eq len="+Length);
				for(int i=0; i<eq_array.Length; i++){
				System.out.print(Integer.toHexString(eq_array[i]&0xff)+":");
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
				c_array=new byte[Length];
				Array.Copy(plain, index, c_array, 0, Length);
				index+=Length;
				/*
				System.out.println("int: c len="+Length);
				for(int i=0; i<c_array.Length; i++){
				System.out.print(Integer.toHexString(c_array[i]&0xff)+":");
				}
				System.out.println("");
				*/
			}
			catch//(Exception e)
			{
				//System.out.println(e);
				return false;
			}
			return true;
		}


		public override byte[] getPublicKeyBlob()
		{
			byte[] foo=base.getPublicKeyBlob();
			if(foo!=null) return foo;

			if(pub_array==null) return null;

			Buffer buf=new Buffer(sshrsa.Length+4+
				pub_array.Length+4+ 
				n_array.Length+4);
			buf.putString(sshrsa);
			buf.putString(pub_array);
			buf.putString(n_array);
			return buf.buffer;
		}

		private static byte[] sshrsa= Util.getBytes( "ssh-rsa" );
		internal override byte[] getKeyTypeName(){return sshrsa;}
		public override int getKeyType(){return RSA;}

		public override int getKeySize(){return key_size; }
		public override void dispose()
		{
			base.dispose();
			pub_array=null;
			prv_array=null;
			n_array=null;

			p_array=null;
			q_array=null;
			ep_array=null;
			eq_array=null;
			c_array=null;
		}
	}

}
