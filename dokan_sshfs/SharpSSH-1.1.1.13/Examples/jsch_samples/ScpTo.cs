using System;
using Tamir.SharpSsh.jsch;
using System.IO;
using System.Windows.Forms;
using System.Text;

/* ScpTo.cs
 * ====================================================================
 * The following example was posted with the original JSch java library,
 * and is translated to C# to show the usage of SharpSSH JSch API
 * ====================================================================
 * */
namespace sharpSshTest.jsch_samples
{
	/// <summary>
	/// This program will demonstrate the file transfer from local to remote.
	/// You will be asked passwd. 
	/// If everything works fine, a local file 'file1' will copied to
	/// 'file2' on 'remotehost'.
	/// </summary>	
	public class ScpTo
	{
		public static void RunExample(String[] arg)
		{
			if(arg.Length!=2)
			{
				Console.WriteLine("usage: java ScpTo file1 user@remotehost:file2");
				Environment.Exit(-1);
			}      

			try
			{

				String lfile=arg[0];
				String user=arg[1].Substring(0, arg[1].IndexOf('@'));
				arg[1]=arg[1].Substring(arg[1].IndexOf('@')+1);
				String host=arg[1].Substring(0, arg[1].IndexOf(':'));
				String rfile=arg[1].Substring(arg[1].IndexOf(':')+1);

				JSch jsch=new JSch();
				Session session=jsch.getSession(user, host, 22);

				// username and password will be given via UserInfo interface.
				UserInfo ui=new MyUserInfo();
				session.setUserInfo(ui);
				session.connect();


				// exec 'scp -t rfile' remotely
				String command="scp -p -t "+rfile;
				Channel channel=session.openChannel("exec");
				((ChannelExec)channel).setCommand(command);

				// get I/O streams for remote scp
				Stream outs=channel.getOutputStream();
				Stream ins=channel.getInputStream();

				channel.connect();

				byte[] tmp=new byte[1];

				if(checkAck(ins)!=0)
				{
					Environment.Exit(0);
				}

				// send "C0644 filesize filename", where filename should not include '/'
								
				int filesize=(int)(new FileInfo(lfile)).Length;
				command="C0644 "+filesize+" ";
				if(lfile.LastIndexOf('/')>0)
				{
					command+=lfile.Substring(lfile.LastIndexOf('/')+1);
				}
				else
				{
					command+=lfile;
				}
				command+="\n";
				byte[] buff = Util.getBytes(command);
				outs.Write(buff, 0, buff.Length); outs.Flush();

				if(checkAck(ins)!=0)
				{
					Environment.Exit(0);
				}

				// send a content of lfile
				FileStream fis=File.OpenRead(lfile);
				byte[] buf=new byte[1024];
				while(true)
				{
					int len=fis.Read(buf, 0, buf.Length);
					if(len<=0) break;
					outs.Write(buf, 0, len); outs.Flush();
					Console.Write("#");
				}

				// send '\0'
				buf[0]=0; outs.Write(buf, 0, 1); outs.Flush();
				Console.Write(".");

				if(checkAck(ins)!=0)
				{
					Environment.Exit(0);
				}
				Console.WriteLine("OK");
				Environment.Exit(0);
			}
			catch(Exception e)
			{
				Console.WriteLine(e);
			}
		}

		static int checkAck(Stream ins) 
		{
			Console.Write(".");
			int b=ins.ReadByte();
			Console.Write(".");
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

		public class MyUserInfo : UserInfo
		{
			public String getPassword(){ return passwd; }
			public bool promptYesNo(String str)
			{
				DialogResult returnVal = MessageBox.Show(
					str,
					"SharpSSH",
					MessageBoxButtons.YesNo,
					MessageBoxIcon.Warning);
				return (returnVal==DialogResult.Yes);
			}
  
			String passwd;
			

			public String getPassphrase(){ return null; }
			public bool promptPassphrase(String message){ return true; }
			public bool promptPassword(String message)
			{
				InputForm passwordField=new InputForm();
				passwordField.Text = message;
				passwordField.PasswordField = true;

				if ( passwordField.PromptForInput() )
				{
					passwd=passwordField.getText();
					return true;
				}
				else{ return false; }
			}
			public void showMessage(String message)
			{
				MessageBox.Show(
					message,
					"SharpSSH",
					MessageBoxButtons.OK,
					MessageBoxIcon.Asterisk);
			}
		}

	}

}
