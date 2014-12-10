using System;

namespace Tamir.SharpSsh.jsch.jce
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

	public class SignatureRSA : Tamir.SharpSsh.jsch.SignatureRSA
	{

		//java.security.Signature signature;	
		//  KeyFactory keyFactory;
		System.Security.Cryptography.RSAParameters RSAKeyInfo;
		System.Security.Cryptography.SHA1CryptoServiceProvider sha1;
		System.Security.Cryptography.CryptoStream cs;

		public void init()
		{
			//    signature=java.security.Signature.getInstance("SHA1withRSA");
			//    keyFactory=KeyFactory.getInstance("RSA");
			sha1 = new System.Security.Cryptography.SHA1CryptoServiceProvider();
			cs = new System.Security.Cryptography.CryptoStream(System.IO.Stream.Null, sha1, System.Security.Cryptography.CryptoStreamMode.Write);
		}     
		public void setPubKey(byte[] e, byte[] n) 
		{
			//    RSAPublicKeySpec rsaPubKeySpec = 
			//	new RSAPublicKeySpec(new BigInteger(n),
			//			     new BigInteger(e));
			//    PublicKey pubKey=keyFactory.generatePublic(rsaPubKeySpec);
			//    signature.initVerify(pubKey);
			RSAKeyInfo.Modulus =  Util.stripLeadingZeros( n );
			RSAKeyInfo.Exponent =  e;
//			Util.Dump("C:\\e.bin", e);
//			Util.Dump("C:\\n.bin", n);
		}
		public void setPrvKey(byte[] d, byte[] n) 
		{
			//    RSAPrivateKeySpec rsaPrivKeySpec = 
			//	new RSAPrivateKeySpec(new BigInteger(n),
			//			      new BigInteger(d));
			//    PrivateKey prvKey = keyFactory.generatePrivate(rsaPrivKeySpec);
			//    signature.initSign(prvKey);

			RSAKeyInfo.D =  d ;
			RSAKeyInfo.Modulus =  n ;
		}

		public void setPrvKey(byte[] e, byte[] n, byte[] d,  byte[] p, byte[] q, byte[] dp, byte[] dq, byte[] c)
		{
			RSAKeyInfo.Exponent = e ;
			RSAKeyInfo.D =  Util.stripLeadingZeros( d ) ;
			RSAKeyInfo.Modulus =  Util.stripLeadingZeros( n ) ;
			RSAKeyInfo.P = Util.stripLeadingZeros(p);
			RSAKeyInfo.Q = Util.stripLeadingZeros(q);
			RSAKeyInfo.DP = Util.stripLeadingZeros(dp);
			RSAKeyInfo.DQ = Util.stripLeadingZeros(dq);
			RSAKeyInfo.InverseQ = Util.stripLeadingZeros(c);
		}

		public void setPrvKey(System.Security.Cryptography.RSAParameters keyInfo) 
		{
			//    RSAPrivateKeySpec rsaPrivKeySpec = 
			//	new RSAPrivateKeySpec(new BigInteger(n),
			//			      new BigInteger(d));
			//    PrivateKey prvKey = keyFactory.generatePrivate(rsaPrivKeySpec);
			//    signature.initSign(prvKey);
			RSAKeyInfo = keyInfo;
		}

		public byte[] sign() 
		{
			//    byte[] sig=signature.sign();      
			//    return sig;
			cs.Close();
			System.Security.Cryptography.RSACryptoServiceProvider RSA = new System.Security.Cryptography.RSACryptoServiceProvider();
			RSA.ImportParameters(RSAKeyInfo);
			System.Security.Cryptography.RSAPKCS1SignatureFormatter RSAFormatter = new System.Security.Cryptography.RSAPKCS1SignatureFormatter(RSA);
			RSAFormatter.SetHashAlgorithm("SHA1");
	  
			byte[] sig = RSAFormatter.CreateSignature( sha1 );
			return  sig;

		
		}
		public void update(byte[] foo)
		{
			//signature.update(foo);
			cs.Write(  foo , 0, foo.Length);
		}
		public bool verify(byte[] sig) 
		{
			cs.Close();
			System.Security.Cryptography.RSACryptoServiceProvider RSA = new System.Security.Cryptography.RSACryptoServiceProvider();
			RSA.ImportParameters(RSAKeyInfo);
			System.Security.Cryptography.RSAPKCS1SignatureDeformatter RSADeformatter = new System.Security.Cryptography.RSAPKCS1SignatureDeformatter(RSA);
			RSADeformatter.SetHashAlgorithm("SHA1");


			long i=0;
			long j=0;
			byte[] tmp;

			//Util.Dump("c:\\sig.bin", sig);

			if(sig[0]==0 && sig[1]==0 && sig[2]==0)
			{
				long i1 = (sig[i++]<<24)&0xff000000;
				long i2 = (sig[i++]<<16)&0x00ff0000;
				long i3 = (sig[i++]<<8)&0x0000ff00;
				long i4 = (sig[i++])&0x000000ff;
				j = i1 | i2 | i3 | i4;

				i+=j;

				i1 = (sig[i++]<<24)&0xff000000;
				i2 = (sig[i++]<<16)&0x00ff0000;
				i3 = (sig[i++]<<8)&0x0000ff00;
				i4 = (sig[i++])&0x000000ff;
				j = i1 | i2 | i3 | i4;

				tmp=new byte[j]; 
				Array.Copy(sig, i, tmp, 0, j); sig=tmp;
			}
			//System.out.println("j="+j+" "+Integer.toHexString(sig[0]&0xff));
			//return signature.verify(sig);
			bool verify = RSADeformatter.VerifySignature(sha1, sig);
			return verify;
		}
	}

}
