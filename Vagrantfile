VAGRANTFILE_API_VERSION = "2"

# Get parameters from environment variables or use defaults
N_WORKER_A = ENV['N_WORKER_A'] ? ENV['N_WORKER_A'].to_i : 3  # Number of Worker A nodes
M_WORKER_B = ENV['M_WORKER_B'] ? ENV['M_WORKER_B'].to_i : 2  # Number of Worker B nodes per Worker A

# Total number of VMs: 1 (master) + N_WORKER_A + N_WORKER_A*M_WORKER_B
TOTAL_VMS = 1 + N_WORKER_A + N_WORKER_A * M_WORKER_B

puts "Configuring cluster with:"
puts "  - 1 master node"
puts "  - #{N_WORKER_A} Worker A nodes"
puts "  - #{M_WORKER_B} Worker B nodes per Worker A (total: #{N_WORKER_A * M_WORKER_B})"
puts "  - Total VMs: #{TOTAL_VMS}"

Vagrant.configure(VAGRANTFILE_API_VERSION) do |config|
  config.vm.box = "debian/bullseye64"
  config.vm.provider "virtualbox" do |v|
    v.memory = 512
    v.cpus = 1
  end
  
  # Shared folder for project data
  config.vm.synced_folder './data', '/opt/data', SharedFoldersEnableSymlinksCreate: false
  
  config.vm.synced_folder './dep', '/opt/dep', SharedFoldersEnableSymlinksCreate: false
  
  config.vm.synced_folder './src', '/opt/src', SharedFoldersEnableSymlinksCreate: false
   
   
	
  # Master node (ID: 0)
  config.vm.define "master" do |master|
    master.vm.hostname = "master"
	master.vm.network "forwarded_port", guest: 8001, host: "#{8001}"
    master.vm.network "private_network",
      ip: "172.16.0.100",
      virtualbox__intnet: "clusternet"
    
    # Only run the post-up script on the master VM after all VMs are up
    master.trigger.after [:up, :reload] do |trigger|
      trigger.ruby do |env, machine|
        puts "Running post-up script for SSH key scanning..."
        # Add master to known_hosts
        system("ssh-keyscan -H master >> ~/.ssh/known_hosts")
        
        # Add Worker A nodes to known_hosts
        (1..N_WORKER_A).each do |i|
          system("ssh-keyscan -H worker-a-#{i} >> ~/.ssh/known_hosts")
        end
        
        # Add Worker B nodes to known_hosts
        (1..N_WORKER_A).each do |a|
          (1..M_WORKER_B).each do |b|
            system("ssh-keyscan -H worker-b-#{a}-#{b} >> ~/.ssh/known_hosts")
          end
        end
      end
    end
  end

  # Worker A nodes (ID: 1 to N)
  1.upto(N_WORKER_A) do |i|
    config.vm.define "worker-a-#{i}" do |worker|
      worker.vm.hostname = "worker-a-#{i}"
      ip = "172.16.0.#{100+i}"
	  worker.vm.network "forwarded_port", guest: 8001, host: "#{8001+i}"
      worker.vm.network "private_network",
        ip: ip,
        virtualbox__intnet: "clusternet"
    end
  end

  # Worker B nodes (ID: N+1 to N+1+N*M)
  1.upto(N_WORKER_A) do |a|
    1.upto(M_WORKER_B) do |b|
      worker_id = N_WORKER_A + (a-1)*M_WORKER_B + b
      config.vm.define "worker-b-#{a}-#{b}" do |worker|
        worker.vm.hostname = "worker-b-#{a}-#{b}"
        ip = "172.16.0.#{100+worker_id}"
		worker.vm.network "forwarded_port", guest: 8001, host: "#{8001+worker_id}"
        worker.vm.network "private_network",
          ip: ip,
          virtualbox__intnet: "clusternet"
      end
    end
  end

  # Common provisioning script for all VMs
  # This script will run on every VM
  config.vm.provision "shell", inline: <<-SCRIPT
    set -x
    if [[ ! -e /etc/.provisioned ]]; then
      # Add all hosts to /etc/hosts
      # Master node
      echo "172.16.0.100 master" >> /etc/hosts
      
      # Worker A nodes
      for i in {1..#{N_WORKER_A}}; do
        ip="172.16.0.$((100+i))"
        echo "$ip worker-a-$i" >> /etc/hosts
      done
      
      # Worker B nodes
      for a in {1..#{N_WORKER_A}}; do
        for b in {1..#{M_WORKER_B}}; do
          worker_id=$((#{N_WORKER_A} + (a-1)*#{M_WORKER_B} + b))
          ip="172.16.0.$((100+worker_id))"
          echo "$ip worker-b-$a-$b" >> /etc/hosts
        done
      done
	  
      echo "    StrictHostKeyChecking no" >> /etc/ssh/ssh_config

      # Generate SSH keys if they don't exist
      if [[ ! -e /vagrant/id_rsa ]]; then
        ssh-keygen -t rsa -f /vagrant/id_rsa -N ""
      fi

      # Install SSH keys
      install -m 600 -o vagrant -g vagrant /vagrant/id_rsa /home/vagrant/.ssh/
      (echo; cat /vagrant/id_rsa.pub) >> /home/vagrant/.ssh/authorized_keys

      # Install required packages for MPI
      apt-get -y update
      apt-get -y install openmpi-bin libopenmpi-dev build-essential g++ libcurl4-openssl-dev
      
      # Create a hosts file for MPI
      echo "master slots=1" > /home/vagrant/hostfile
      for i in {1..#{N_WORKER_A}}; do
        echo "worker-a-$i slots=1" >> /home/vagrant/hostfile
      done
      for a in {1..#{N_WORKER_A}}; do
        for b in {1..#{M_WORKER_B}}; do
          echo "worker-b-$a-$b slots=1" >> /home/vagrant/hostfile
        done
      done
      chown vagrant:vagrant /home/vagrant/hostfile
      
      # Create a README file with instructions
      cat > /home/vagrant/README.txt << EOF
=====================================================
MPI WEB CRAWLER CLUSTER SETUP
=====================================================

Your cluster is configured with:
- 1 master node (ID: 0)
- #{N_WORKER_A} Worker A nodes (ID: 1 to #{N_WORKER_A})
- #{M_WORKER_B} Worker B nodes per Worker A (Total: #{N_WORKER_A * M_WORKER_B})
- Total processes needed: 1 + #{N_WORKER_A} + #{N_WORKER_A}*#{M_WORKER_B} = #{1 + N_WORKER_A + N_WORKER_A * M_WORKER_B}

To run your MPI program:
  mpirun -np #{1 + N_WORKER_A + N_WORKER_A * M_WORKER_B} -hostfile ~/hostfile ./upp2 -n #{N_WORKER_A} -m #{M_WORKER_B}

Make sure your program accepts the parameters:
  -n <N> : Number of Worker A processes
  -m <M> : Number of Worker B processes per Worker A

Example: ./upp2 -n #{N_WORKER_A} -m #{M_WORKER_B}
=====================================================
EOF
      chown vagrant:vagrant /home/vagrant/README.txt
      
      touch /etc/.provisioned
    fi
  SCRIPT
end