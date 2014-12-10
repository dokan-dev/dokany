using System;
using System.IO;
using System.Collections;
using System.Windows.Forms;

/* ScpTo.cs
 * ====================================================================
 * The following example was posted with the original JSch java library,
 * and is translated to C# to show the usage of SharpSSH JSch API
 * ====================================================================
 * */
namespace Tamir.SharpSsh.jsch.examples
{
	/// <summary>
	/// This program will demonstrate the sftp protocol support.
	/// You will be asked username, host and passwd. 
	/// If everything works fine, you will get a prompt 'sftp>'. 
	/// 'help' command will show available command.
	/// In current implementation, the destination path for 'get' and 'put'
	/// commands must be a file, not a directory.
	/// </summary>
	public class Sftp
	{
		public static void Main(String[] arg)
		{
			try
			{
				JSch jsch=new JSch();

				InputForm inForm = new InputForm();
				inForm.Text = "Enter username@hostname";
				inForm.textBox1.Text = Environment.UserName+"@localhost"; 

				if (!inForm.PromptForInput())
				{
					Console.WriteLine("Cancelled");
					return;
				}
				String host = inForm.textBox1.Text;
				String user=host.Substring(0, host.IndexOf('@'));
				host=host.Substring(host.IndexOf('@')+1);

				Session session=jsch.getSession(user, host, 22);

				// username and password will be given via UserInfo interface.
				UserInfo ui=new MyUserInfo();
				session.setUserInfo(ui);

				session.connect();

				Channel channel=session.openChannel("sftp");
				channel.connect();
				ChannelSftp c=(ChannelSftp)channel;

				Stream ins=Console.OpenStandardInput();
				TextWriter outs=Console.Out;

				ArrayList cmds=new ArrayList();
				byte[] buf=new byte[1024];
				int i;
				String str;
				int level=0;

				while(true)
				{
					outs.Write("sftp> ");
					cmds.Clear();
					i=ins.Read(buf, 0, 1024);
					if(i<=0)break;

					i--;
					if(i>0 && buf[i-1]==0x0d)i--;
					//str=Util.getString(buf, 0, i);
					//Console.WriteLine("|"+str+"|");
					int s=0;
					for(int ii=0; ii<i; ii++)
					{
						if(buf[ii]==' ')
						{
							if(ii-s>0){ cmds.Add(Util.getString(buf, s, ii-s)); }
							while(ii<i){if(buf[ii]!=' ')break; ii++;}
							s=ii;
						}
					}
					if(s<i){ cmds.Add(Util.getString(buf, s, i-s)); }
					if(cmds.Count==0)continue;

					String cmd=(String)cmds[0];
					if(cmd.Equals("quit"))
					{
						c.quit();
						break;
					}
					if(cmd.Equals("exit"))
					{
						c.exit();
						break;
					}
					if(cmd.Equals("rekey"))
					{
						session.rekey();
						continue;
					}
					if(cmd.Equals("compression"))
					{
						if(cmds.Count<2)
						{
							outs.WriteLine("compression level: "+level);
							continue;
						}
						try
						{
							level=int.Parse((String)cmds[1]);
							Hashtable config=new Hashtable();
							if(level==0)
							{
								config.Add("compression.s2c", "none");
								config.Add("compression.c2s", "none");
							}
							else
							{
								config.Add("compression.s2c", "zlib,none");
								config.Add("compression.c2s", "zlib,none");
							}
							session.setConfig(config);
						}
						catch{}//(Exception e){}
						continue;
					}
					if(cmd.Equals("cd") || cmd.Equals("lcd"))
					{
						if(cmds.Count<2) continue;
						String path=(String)cmds[1];
						try
						{
							if(cmd.Equals("cd")) c.cd(path);
							else c.lcd(path);
						}
						catch(SftpException e)
						{
							Console.WriteLine(e.message);
						}
						continue;
					}
					if(cmd.Equals("rm") || cmd.Equals("rmdir") || cmd.Equals("mkdir"))
					{
						if(cmds.Count<2) continue;
						String path=(String)cmds[1];
						try
						{
							if(cmd.Equals("rm")) c.rm(path);
							else if(cmd.Equals("rmdir")) c.rmdir(path);
							else c.mkdir(path);
						}
						catch(SftpException e)
						{
							Console.WriteLine(e.message);
						}
						continue;
					}
					if(cmd.Equals("chgrp") || cmd.Equals("chown") || cmd.Equals("chmod"))
					{
						if(cmds.Count!=3) continue;
						String path=(String)cmds[2];
						int foo=0;
						if(cmd.Equals("chmod"))
						{
							byte[] bar=Util.getBytes((String)cmds[1]);
							int k;
							for(int j=0; j<bar.Length; j++)
							{
								k=bar[j];
								if(k<'0'||k>'7'){foo=-1; break;}
								foo<<=3;
								foo|=(k-'0');
							}
							if(foo==-1)continue;
						}
						else
						{
							try{foo=int.Parse((String)cmds[1]);}
							catch{}//(Exception e){continue;}
						}
						try
						{
							if(cmd.Equals("chgrp")){ c.chgrp(foo, path); }
							else if(cmd.Equals("chown")){ c.chown(foo, path); }
							else if(cmd.Equals("chmod")){ c.chmod(foo, path); }
						}
						catch(SftpException e)
						{
							Console.WriteLine(e.message);
						}
						continue;
					}
					if(cmd.Equals("pwd") || cmd.Equals("lpwd"))
					{
						str=(cmd.Equals("pwd")?"Remote":"Local");
						str+=" working directory: ";
						if(cmd.Equals("pwd")) str+=c.pwd();
						else str+=c.lpwd();
						outs.WriteLine(str);
						continue;
					}
					if(cmd.Equals("ls") || cmd.Equals("dir"))
					{
						String path=".";
						if(cmds.Count==2) path=(String)cmds[1];
						try
						{
							ArrayList vv=c.ls(path);
							if(vv!=null)
							{
								for(int ii=0; ii<vv.Count; ii++)
								{
									object obj = vv[ii];
									if(obj is ChannelSftp.LsEntry)
										outs.WriteLine(vv[ii]);
								}
							}
						}
						catch(SftpException e)
						{
							Console.WriteLine(e.message);
						}
						continue;
					}
					if(cmd.Equals("lls") || cmd.Equals("ldir"))
					{
						String path=".";
						if(cmds.Count==2) path=(String)cmds[1];
						try
						{
							//java.io.File file=new java.io.File(path);
							if(!File.Exists(path))
							{
								outs.WriteLine(path+": No such file or directory");
								continue; 
							}
							if(Directory.Exists(path))
							{
								String[] list=Directory.GetDirectories(path);
								for(int ii=0; ii<list.Length; ii++)
								{
									outs.WriteLine(list[ii]);
								}
								continue;
							}
							outs.WriteLine(path);
						}
						catch(Exception e)
						{
							Console.WriteLine(e);
						}
						continue;
					}
					if(cmd.Equals("get") || 
						cmd.Equals("get-resume") || cmd.Equals("get-append") || 
						cmd.Equals("put") || 
						cmd.Equals("put-resume") || cmd.Equals("put-append")
						)
					{
						if(cmds.Count!=2 && cmds.Count!=3) continue;
						String p1=(String)cmds[1];
						//	  String p2=p1;
						String p2=".";
						if(cmds.Count==3)p2=(String)cmds[2];
						try
						{
							SftpProgressMonitor monitor=new MyProgressMonitor();
							if(cmd.StartsWith("get"))
							{
								int mode=ChannelSftp.OVERWRITE;
								if(cmd.Equals("get-resume")){ mode=ChannelSftp.RESUME; }
								else if(cmd.Equals("get-append")){ mode=ChannelSftp.APPEND; } 
								c.get(p1, p2, monitor, mode);
							}
							else
							{ 
								int mode=ChannelSftp.OVERWRITE;
								if(cmd.Equals("put-resume")){ mode=ChannelSftp.RESUME; }
								else if(cmd.Equals("put-append")){ mode=ChannelSftp.APPEND; } 
								c.put(p1, p2, monitor, mode); 
							}
						}
						catch(SftpException e)
						{
							Console.WriteLine(e.message);
						}
						continue;
					}
					if(cmd.Equals("ln") || cmd.Equals("symlink") || cmd.Equals("rename"))
					{
						if(cmds.Count!=3) continue;
						String p1=(String)cmds[1];
						String p2=(String)cmds[2];
						try
						{
							if(cmd.Equals("rename")) c.rename(p1, p2);
							else c.symlink(p1, p2);
						}
						catch(SftpException e)
						{
							Console.WriteLine(e.message);
						}
						continue;
					}
					if(cmd.Equals("stat") || cmd.Equals("lstat"))
					{
						if(cmds.Count!=2) continue;
						String p1=(String)cmds[1];
						SftpATTRS attrs=null;
						try
						{
							if(cmd.Equals("stat")) attrs=c.stat(p1);
							else attrs=c.lstat(p1);
						}
						catch(SftpException e)
						{
							Console.WriteLine(e.message);
						}
						if(attrs!=null)
						{
							outs.WriteLine(attrs);
						}
						else
						{
						}
						continue;
					}
					if(cmd.Equals("version"))
					{
						outs.WriteLine("SFTP protocol version "+c.version());
						continue;
					}
					if(cmd.Equals("help") || cmd.Equals("?"))
					{
						outs.WriteLine(help);
						continue;
					}
					outs.WriteLine("unimplemented command: "+cmd);
				}
				session.disconnect();
			}
			catch(Exception e)
			{
				Console.WriteLine(e);
			}			
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
			InputForm passwordField=new InputForm();

			public String getPassphrase(){ return null; }
			public bool promptPassphrase(String message){ return true; }
			public bool promptPassword(String message)
			{
				InputForm inForm = new InputForm();
				inForm.Text = message;
				inForm.PasswordField = true;
				
				if ( inForm.PromptForInput() )
				{
					passwd=inForm.getText();
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

		public class MyProgressMonitor : SftpProgressMonitor
		{
			private ConsoleProgressBar bar;
			private long c = 0;
			private long max = 0;
			private long percent=-1;
			int elapsed=-1;

			System.Timers.Timer timer;

			public override void init(int op, String src, String dest, long max)
			{
				bar = new ConsoleProgressBar();
				this.max=max;
				elapsed=0;
				timer=new System.Timers.Timer(1000);
				timer.Start();
				timer.Elapsed += new System.Timers.ElapsedEventHandler(timer_Elapsed);			
			}
			public override bool count(long c)
			{
				this.c += c;
				if(percent>=this.c*100/max){ return true; }
				percent=this.c*100/max;

				string note = ("Transfering... [Elapsed time: "+elapsed+"]");   

				bar.Update((int)this.c, (int)max, note);
				return true;
			}
			public override void end()
			{
				timer.Stop();
				timer.Dispose();
				string note = ("Done in "+elapsed+" seconds!");   
				bar.Update((int)this.c, (int)max, note);
				bar=null;
			}

			private void timer_Elapsed(object sender, System.Timers.ElapsedEventArgs e)
			{
				this.elapsed++;
			}
		}

		private static String help =
			"      Available commands:\n"+
			"      * means unimplemented command.\n"+
			"cd path                       Change remote directory to 'path'\n"+
			"lcd path                      Change local directory to 'path'\n"+
			"chgrp grp path                Change group of file 'path' to 'grp'\n"+
			"chmod mode path               Change permissions of file 'path' to 'mode'\n"+
			"chown own path                Change owner of file 'path' to 'own'\n"+
			"help                          Display this help text\n"+
			"get remote-path [local-path]  Download file\n"+
			"get-resume remote-path [local-path]  Resume to download file.\n"+
			"get-append remote-path [local-path]  Append remote file to local file\n"+
			"*lls [ls-options [path]]      Display local directory listing\n"+
			"ln oldpath newpath            Symlink remote file\n"+
			"*lmkdir path                  Create local directory\n"+
			"lpwd                          Print local working directory\n"+
			"ls [path]                     Display remote directory listing\n"+
			"*lumask umask                 Set local umask to 'umask'\n"+
			"mkdir path                    Create remote directory\n"+
			"put local-path [remote-path]  Upload file\n"+
			"put-resume local-path [remote-path]  Resume to upload file\n"+
			"put-append local-path [remote-path]  Append local file to remote file.\n"+
			"pwd                           Display remote working directory\n"+
			"stat path                     Display info about path\n"+
			"exit                          Quit sftp\n"+
			"quit                          Quit sftp\n"+
			"rename oldpath newpath        Rename remote file\n"+
			"rmdir path                    Remove remote directory\n"+
			"rm path                       Delete remote file\n"+
			"symlink oldpath newpath       Symlink remote file\n"+
			"rekey                         Key re-exchanging\n"+
			"compression level             Packet compression will be enabled\n"+
			"version                       Show SFTP version\n"+
			"?                             Synonym for help";
	}

}
