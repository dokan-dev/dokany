using System.IO;
using System.Collections;
using Tamir.SharpSsh.jsch;
using Tamir.Streams;
using System.Text;

namespace Tamir
{
	public class SshHelper
	{
	
		private StreamReader reader;
		private PipedOutputStream writer_po;
		private Session session;
		private ChannelShell channel;
		private string host;
	
		public SshHelper(string host, string username, string password)
		{
			this.host = host;
			JSch jsch=new JSch();
			Session session=jsch.getSession(username, host, 22);
			session.setPassword( password );
		
			Hashtable config=new Hashtable();
			config.Add("StrictHostKeyChecking", "no");
			session.setConfig(config);
		
			session.connect();
		
			channel=(ChannelShell)session.openChannel("shell");
		
			writer_po = new PipedOutputStream();
			PipedInputStream writer_pi = new PipedInputStream( writer_po );
		  
			PipedInputStream reader_pi = new PipedInputStream();
			PipedOutputStream reader_po = new PipedOutputStream( reader_pi );
			reader = new StreamReader (reader_pi,Encoding.UTF8);
		  
		  
			channel.setInputStream( writer_pi );
			channel.setOutputStream( reader_po );
	      
			channel.connect();
			channel.setPtySize(132, 132, 1024, 768);
	      
		}
	
		public void Write(string data) 
		{
			data += "\n";
			writer_po.write( Util.getBytes(data ));
		}
	
		public string WriteAndRead(string data)
		{
			Write( data );
			return ReadResponse();
		}
	
		public void SendCtrlC() 
		{
			byte ASCII_CTRL_C = 3;
			byte[] ctrlc = { ASCII_CTRL_C };
			writer_po.write( ctrlc );
		}
	
		public void SendCtrlZ() 
		{
			byte ASCII_CTRL_Z = 26;
			byte[] ctrlz = { ASCII_CTRL_Z };
			writer_po.write( ctrlz );
		}
	
		public string ReadResponse()
		{
			//		return ReadLine();
			return ReadTest();
		}
	
		public string ReadBuffer(int size)
		{
			string res = "";
			int buff;
			try 
			{
				buff = reader.Read();
				while( ((char)buff != '#') && (size != 0) )
				{
					res += (char)buff;
					size--;
					if (res.EndsWith("More--"))
						writer_po.write(Util.getBytes( " ") );
					buff = reader.Read();
				}
				if(buff != '\n')
					res += (char)buff;
			}
			catch//(Exception exc) 
			{  		
			}  
			res = RemoveJunk( res );
			return res;
		}
	
		public void Close()
		{		
			channel.disconnect();
			writer_po.close();
			reader.Close();
		}
  
//		private string ReadLine()
//		{
//			string res = "";
//			int buff;
//			try 
//			{
//				buff = reader.ReadByte();
//				while((char)buff != '#')
//				{
//					res += (char)buff;	  		
//					if (res.EndsWith("More--"))
//						writer_po.write( Util.getBytes(" ") );
//					buff = reader.ReadByte();
//				}
//				if(buff != '\n')
//					res += (char)buff;
//			}
//			catch//Exception exc) 
//			{ 		
//			}
//			res = RemoveJunk( res );
//			return res;
//		}
  
		private string ReadTest()
		{
			StringBuilder res = new StringBuilder();
			char[] buff = new char[1024];
			int count = 0;
  	
			try 
			{
				count = reader.Read(buff, 0, buff.Length);
				//ArrayList lstBuff = new ArrayList(java.util.Arrays.asList(buff));
				while(!Util.ArrayContains(buff, '#', count))
				{
					//	  	while(buff[count-2]!='#' ){
					res.Append( buff, 0, count ); 
					if ( (res.ToString().EndsWith("More--"))||(res.ToString().EndsWith("More--[m")))
						writer_po.write( Util.getBytes( " ") );
					count = reader.Read(buff, 0, buff.Length);
					count++;
					count--;
					//lstBuff = new ArrayList(java.util.Arrays.asList(buff));
				}
				if(buff[count-1] != '\n')
					res.Append( buff, 0, count ); 
				else
					res.Append( buff, 0, count-1 ); 
			}
			catch//(Exception exc) 
			{  		
			}  
			return RemoveJunk( res ).ToString();
		}
  
		private static string RemoveJunk(string str)
		{
			string[] junk = new string[]{"[132;1H", "[24;1H", "[K", "[7m", "[m", " \b", "--More--", "\r", "\n   "};
  	
			for( int i=0; i<junk.Length; i++ )
			{
				string match = junk[i];
				if (match=="\n   ")
					str = Replace(str, match, "   ");
				else
					str = Replace(str, match, "");
	   	
			}
			return str;
		}
  
		private static StringBuilder RemoveJunk(StringBuilder str)
		{
			string[] junk = new string[]{"[132;1H", "[24;1H", "[K", "[7m", "[m", " \b", "--More--", "\n   "};
  	
			for( int i=0; i<junk.Length; i++ )
			{
				string match = junk[i];
				if (match=="\n   ")
					str = Replace(str, match, "   ");
				else
					str = Replace(str, match, "");
	   	
			}
			return str;
		}
  
		private static string Replace(string str, string toReplace, string replcaeWith)
		{
			int index = (str.IndexOf( toReplace ));
			while (index != -1)
			{
				string pre = str.Substring(0, index);
				string post = str.Substring(index+toReplace.Length, str.Length-index-toReplace.Length);
				str = pre + replcaeWith + post;
				index = (str.IndexOf( toReplace ));
			}
			return str;
		}
		private static StringBuilder Replace(StringBuilder str, string toReplace, string replcaeWith)
		{
			return str.Replace(toReplace, replcaeWith);
		}

	}
}

