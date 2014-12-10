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

	public class Buffer
	{
		static byte[] tmp=new byte[4];
		internal byte[] buffer;
		internal int index;
		internal int s;
		public Buffer(int size)
		{
			buffer=new byte[size];
			index=0;
			s=0;
		}
		public Buffer(byte[] buffer)
		{
			this.buffer=buffer;
			index=0;
			s=0;
		}
		public Buffer():this(1024*10*2){ }
		public void putByte(byte foo)
		{
			buffer[index++]=foo;
		}
		public void putByte(byte[] foo) 
		{
			putByte(foo, 0, foo.Length);
		}
		public void putByte(byte[] foo, int begin, int length) 
		{
			Array.Copy(foo, begin, buffer, index, length);
			index+=length;
		}
		public void putString(byte[] foo)
		{
			putString(foo, 0, foo.Length);
		}
		public void putString(byte[] foo, int begin, int length) 
		{
			putInt(length);
			putByte(foo, begin, length);
		}
		public void putInt(int v) 
		{
			uint val = (uint)v;
			tmp[0]=(byte)(val >> 24);
			tmp[1]=(byte)(val >> 16);
			tmp[2]=(byte)(val >> 8);
			tmp[3]=(byte)(val);
			Array.Copy(tmp, 0, buffer, index, 4);
			index+=4;
		}
		public void putLong(long v) 
		{
			ulong val = (ulong)v;
			tmp[0]=(byte)(val >> 56);
			tmp[1]=(byte)(val >> 48);
			tmp[2]=(byte)(val >>40);
			tmp[3]=(byte)(val >> 32);
			Array.Copy(tmp, 0, buffer, index, 4);
			tmp[0]=(byte)(val >> 24);
			tmp[1]=(byte)(val >> 16);
			tmp[2]=(byte)(val >> 8);
			tmp[3]=(byte)(val);
			Array.Copy(tmp, 0, buffer, index+4, 4);
			index+=8;
		}
		internal void skip(int n) 
		{
			index+=n;
		}
		internal void putPad(int n) 
		{
			while(n>0)
			{
				buffer[index++]=(byte)0;
				n--;
			}
		}
		public void putMPInt(byte[] foo)
		{
			int i=foo.Length;
			if((foo[0]&0x80)!=0)
			{
				i++;
				putInt(i);
				putByte((byte)0);
			}
			else
			{
				putInt(i);
			}
			putByte(foo);
		}
		public int getLength()
		{
			return index-s;
		}
		public int getOffSet()
		{
			return s;
		}
		public void setOffSet(int s)
		{
			this.s=s;
		}
		public long getLong()
		{
			long foo = getInt()&0xffffffffL;
			foo = ((foo<<32)) | (getInt()&0xffffffffL);
			return foo;
		}
		public int getInt()
		{
			uint foo = (uint) getShort();
			foo = ((foo<<16)&0xffff0000) | ((uint)getShort()&0xffff);
			return (int)foo;
		}
		internal int getShort() 
		{
			int foo = getByte();
			foo = ((foo<<8)&0xff00)|(getByte()&0xff);
			return foo;
		}
		public int getByte() 
		{
			return (buffer[s++]&0xff);
		}
		public void getByte(byte[] foo) 
		{
			getByte(foo, 0, foo.Length);
		}
		void getByte(byte[] foo, int start, int len) 
		{
			Array.Copy(buffer, s, foo, start, len); 
			s+=len;
		}
		public int getByte(int len) 
		{
			int foo=s;
			s+=len;
			return foo;
		}
		public byte[] getMPInt() 
		{
			int i=getInt();
			byte[] foo=new byte[i];
			getByte(foo, 0, i);
			return foo;
		}
		public byte[] getMPIntBits() 
		{
			int bits=getInt();
			int bytes=(bits+7)/8;
			byte[] foo=new byte[bytes];
			getByte(foo, 0, bytes);
			if((foo[0]&0x80)!=0)
			{
				byte[] bar=new byte[foo.Length+1];
				bar[0]=0; // ??
				Array.Copy(foo, 0, bar, 1, foo.Length);
				foo=bar;
			}
			return foo;
		}
		public byte[] getString() 
		{
			int i=getInt();
			byte[] foo=new byte[i];
			getByte(foo, 0, i);
			return foo;
		}
		internal byte[] getString(int[]start, int[]len) 
		{
			int i=getInt();
			start[0]=getByte(i);
			len[0]=i;
			return buffer;
		}
		public void reset()
		{
			index=0;
			s=0;
		}
		public void shift()
		{
			if(s==0)return;
			Array.Copy(buffer, s, buffer, 0, index-s);
			index=index-s;
			s=0;
		}
		internal void rewind()
		{
			s=0;
		}

		/*
		  static String[] chars={
			"0","1","2","3","4","5","6","7","8","9", "a","b","c","d","e","f"
		  };
		  static void dump_buffer(){
			int foo;
			for(int i=0; i<tmp_buffer_index; i++){
				foo=tmp_buffer[i]&0xff;
			System.out.print(chars[(foo>>>4)&0xf]);
			System.out.print(chars[foo&0xf]);
				if(i%16==15){
				  System.out.println("");
			  continue;
			}
				if(i>0 && i%2==1){
				  System.out.print(" ");
			}
			}
			System.out.println("");
		  }
		  static void dump(byte[] b){
			dump(b, 0, b.length);
		  }
		  static void dump(byte[] b, int s, int l){
			for(int i=s; i<s+l; i++){
			  System.out.print(Integer.toHexString(b[i]&0xff)+":");
			}
			System.out.println("");
		  }
		*/

	}

}
