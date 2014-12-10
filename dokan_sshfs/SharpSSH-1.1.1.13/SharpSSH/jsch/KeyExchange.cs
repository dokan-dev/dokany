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

	public abstract class KeyExchange
	{

		internal const int PROPOSAL_KEX_ALGS=0;
		internal const int PROPOSAL_SERVER_HOST_KEY_ALGS=1;
		internal const int PROPOSAL_ENC_ALGS_CTOS=2;
		internal const int PROPOSAL_ENC_ALGS_STOC=3;
		internal const int PROPOSAL_MAC_ALGS_CTOS=4;
		internal const int PROPOSAL_MAC_ALGS_STOC=5;
		internal const int PROPOSAL_COMP_ALGS_CTOS=6;
		internal const int PROPOSAL_COMP_ALGS_STOC=7;
		internal const int PROPOSAL_LANG_CTOS=8;
		internal const int PROPOSAL_LANG_STOC=9;
		internal const int PROPOSAL_MAX=10;

		//static String kex_algs="diffie-hellman-group-exchange-sha1"+
		//                       ",diffie-hellman-group1-sha1";

		//static String kex="diffie-hellman-group-exchange-sha1";
		internal static String kex="diffie-hellman-group1-sha1";
		internal static String server_host_key="ssh-rsa,ssh-dss";
		internal static String enc_c2s="blowfish-cbc";
		internal static String enc_s2c="blowfish-cbc";
		internal static String mac_c2s="hmac-md5";     // hmac-md5,hmac-sha1,hmac-ripemd160,
		// hmac-sha1-96,hmac-md5-96
		internal static String mac_s2c="hmac-md5";
		//static String comp_c2s="none";        // zlib
		//static String comp_s2c="none";
		internal static String lang_c2s="";
		internal static String lang_s2c="";

		public const int STATE_END=0;

		public Tamir.SharpSsh.java.String[] _guess=null;
		protected Session session=null;
		protected HASH sha=null;
		protected byte[] K=null;
		protected byte[] H=null;
		protected byte[] K_S=null;

		public abstract void init(Session session, 
			byte[] V_S, byte[] V_C, byte[] I_S, byte[] I_C);
		public abstract bool next(Buffer buf);
		public abstract String getKeyType();
		public abstract int getState();

		/*
		void dump(byte[] foo){
		  for(int i=0; i<foo.length; i++){
			if((foo[i]&0xf0)==0)System.out.print("0");
			System.out.print(Integer.toHexString(foo[i]&0xff));
			if(i%16==15){System.out.println(""); continue;}
			if(i%2==1)System.out.print(" ");
		  }
		} 
		*/

		internal static Tamir.SharpSsh.java.String[] guess(byte[]I_S, byte[]I_C)
		{
			//System.out.println("guess: ");
			Tamir.SharpSsh.java.String[] guess=new Tamir.SharpSsh.java.String[PROPOSAL_MAX];
			Buffer sb=new Buffer(I_S); sb.setOffSet(17);
			Buffer cb=new Buffer(I_C); cb.setOffSet(17);

			for(int i=0; i<PROPOSAL_MAX; i++)
			{
				byte[] sp=sb.getString();  // server proposal
				byte[] cp=cb.getString();  // client proposal

				//System.out.println("server-proposal: |"+new String(sp)+"|");
				//System.out.println("client-proposal: |"+new String(cp)+"|");

				int j=0;
				int k=0;
				//System.out.println(new String(cp));
			//loop(using BREAK instead):
				while(j<cp.Length)
				{
					while(j<cp.Length && cp[j]!=',')j++; 
					if(k==j) return null;
					String algorithm=Util.getString(cp, k, j-k);
					//System.out.println("algorithm: "+algorithm);
					int l=0;
					int m=0;
					while(l<sp.Length)
					{
						while(l<sp.Length && sp[l]!=',')l++; 
						if(m==l) return null;
						//System.out.println("  "+new String(sp, m, l-m));
						if(algorithm.Equals(Util.getString(sp, m, l-m)))
						{
							guess[i]=algorithm;
							//System.out.println("  "+algorithm);
							goto BREAK;
						}
						l++;
						m=l;
					}	
					j++;
					k=j;
				}
			BREAK:
				if(j==0)
				{
					guess[i]="";
				}
				else if(guess[i]==null)
				{
					//System.out.println("  fail");
					return null;
				}
			}

			//    for(int i=0; i<PROPOSAL_MAX; i++){
			//      System.out.println("guess: ["+guess[i]+"]");
			//    }

			return guess;
		}

		public String getFingerPrint()
		{
			HASH hash=null;
			try
			{
				Type t=Type.GetType(session.getConfig("md5"));
				hash=(HASH)(Activator.CreateInstance(t));
			}
			catch(Exception e){ Console.Error.WriteLine("getFingerPrint: "+e); }
			return Util.getFingerPrint(hash, getHostKey());
		}
		internal byte[] getK(){ return K; }
		internal byte[] getH(){ return H; }
		internal HASH getHash(){ return sha; }
		internal byte[] getHostKey(){ return K_S; }
	}
}
