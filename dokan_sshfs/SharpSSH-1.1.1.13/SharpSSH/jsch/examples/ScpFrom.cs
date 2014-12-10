using System;
using Tamir.SharpSsh.jsch;
using System.IO;
using System.Windows.Forms;
using System.Text;

/* ScpFrom.cs
 * ====================================================================
 * The following example was posted with the original JSch java library,
 * and is translated to C# to show the usage of SharpSSH JSch API
 * ====================================================================
 * */
namespace Tamir.SharpSsh.jsch.examples
{
	/// <summary>
	/// This program will demonstrate the file transfer from remote to local
	/// You will be asked passwd. 
	/// If everything works fine, a file 'file1' on 'remotehost' will copied to
	/// local 'file1'.
	/// </summary>
	public class ScpFrom
	{	
		public static void Main(String[] arg)
		{
			if(arg.Length!=2)
			{
				Console.WriteLine("usage: java ScpFrom user@remotehost:file1 file2");
				return;
			}      

			try
			{
				String user=arg[0].Substring(0, arg[0].IndexOf('@'));
				arg[0]=arg[0].Substring(arg[0].IndexOf('@')+1);
				String host=arg[0].Substring(0, arg[0].IndexOf(':'));
				String rfile=arg[0].Substring(arg[0].IndexOf(':')+1);
				String lfile=arg[1];

				String prefix=null;
				if(Directory.Exists(lfile))
				{
					prefix=lfile+Path.DirectorySeparatorChar;
				}

				JSch jsch=new JSch();
				Session session=jsch.getSession(user, host, 22);

				// username and password will be given via UserInfo interface.
				UserInfo ui=new MyUserInfo();
				session.setUserInfo(ui);
				session.connect();

				// exec 'scp -f rfile' remotely
				String command="scp -f "+rfile;
				Channel channel=session.openChannel("exec");
				((ChannelExec)channel).setCommand(command);

				// get I/O streams for remote scp
				Stream outs=channel.getOutputStream();
				Stream ins=channel.getInputStream();

				channel.connect();

				byte[] buf=new byte[1024];

				// send '\0'
				buf[0]=0; outs.Write(buf, 0, 1); outs.Flush();

				while(true)
				{
					int c=checkAck(ins);
					if(c!='C')
					{
						break;
					}

					// read '0644 '
					ins.Read(buf, 0, 5);

					int filesize=0;
					while(true)
					{
						ins.Read(buf, 0, 1);
						if(buf[0]==' ')break;
						filesize=filesize*10+(buf[0]-'0');
					}

					String file=null;
					for(int i=0;;i++)
					{
						ins.Read(buf, i, 1);
						if(buf[i]==(byte)0x0a)
						{
							file=Util.getString(buf, 0, i);
							break;
						}
					}

					//Console.WriteLine("filesize="+filesize+", file="+file);

					// send '\0'
					buf[0]=0; outs.Write(buf, 0, 1); outs.Flush();

					// read a content of lfile
					FileStream fos=File.OpenWrite(prefix==null ? 
					lfile :
						prefix+file);
					int foo;
					while(true)
					{
						if(buf.Length<filesize) foo=buf.Length;
						else foo=filesize;
						ins.Read(buf, 0, foo);
						fos.Write(buf, 0, foo);
						filesize-=foo;
						if(filesize==0) break;
					}
					fos.Close();

					byte[] tmp=new byte[1];

					if(checkAck(ins)!=0)
					{
						Environment.Exit(0);
					}

					// send '\0'
					buf[0]=0; outs.Write(buf, 0, 1); outs.Flush();
				}
				Environment.Exit(0);
			}
			catch(Exception e)
			{
				Console.WriteLine(e);
			}
		}

		static int checkAck(Stream ins) 
		{
			int b=ins.ReadByte();
			// b may be 0 for success,
			//          1 for error,
			//          2 for fatal error,
			//          -1
			if(b==0) return b;
			if(b==-1) return b;

			if(b==1 || b==2)
			{
				StringBuilder sb=new StringBuilder();
				int c;
				do 
				{
					c=ins.ReadByte();
					sb.Append((char)c);
				}
				while(c!='\n');
				if(b==1)
				{ // error
					Console.Write(sb.ToString());
				}
				if(b==2)
				{ // fatal error
					Console.Write(sb.ToString());
				}
			}
			return b;
		}

		/// <summary>
		/// A user info for getting user data
		/// </summary>
		public class MyUserInfo : UserInfo
		{
			/// <summary>
			/// Holds the user password
			/// </summary>
			private String passwd;

			/// <summary>
			/// Returns the user password
			/// </summary>
			public String getPassword(){ return passwd; }

			/// <summary>
			/// Prompt the user for a Yes/No input
			/// </summary>
			public bool promptYesNo(String str)
			{
				return InputForm.PromptYesNo(str);
			}
			
			/// <summary>
			/// Returns the user passphrase (passwd for the private key file)
			/// </summary>
			public String getPassphrase(){ return null; }

			/// <summary>
			/// Prompt the user for a passphrase (passwd for the private key file)
			/// </summary>
			public bool promptPassphrase(String message){ return true; }

			/// <summary>
			/// Prompt the user for a password
			/// </summary>\
			public bool promptPassword(String message)
			{
				passwd=InputForm.GetUserInput(message, true);
				return true;
			}

			/// <summary>
			/// Shows a message to the user
			/// </summary>
			public void showMessage(String message)
			{
				InputForm.ShowMessage(message);
			}
		}
	}
}
