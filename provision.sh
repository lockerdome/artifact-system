#!/bin/bash
set -e  # Exit immediately if a command exits with a non-zero status

cleanup() {
    rm -f lakefs-signer.json build-script.sh runtime-startup.sh batch-request.json
    echo ">>> Cleanup complete."
}
trap cleanup EXIT

# --- CONFIGURATION ---
export PROJECT_ID="your-project-id"  # <--- REPLACE THIS
export REGION="us-central1"
export VPC_NAME="lakefs-vpc"
export SUBNET_NAME="lakefs-subnet"
export SUBNET_RANGE="10.0.0.0/20"    # Range for your VMs
export PSA_RANGE="10.100.0.0/16"     # Range for Cloud SQL (Service Peering)
export DB_INSTANCE_NAME="lakefs-db"

# Set the project context
gcloud config set project "$PROJECT_ID"

echo ">>> Starting Part 1: Foundation setup for Project: $PROJECT_ID in $REGION"

# 1. Enable APIs
# Note: Enabling APIs can take a few minutes to propagate.
echo ">>> Enabling required APIs..."
gcloud services enable \
    compute.googleapis.com \
    sqladmin.googleapis.com \
    servicenetworking.googleapis.com \
    secretmanager.googleapis.com \
    dataproc.googleapis.com

# 2. Create VPC and Subnet
echo ">>> Creating VPC: $VPC_NAME..."
gcloud compute networks create "$VPC_NAME" \
    --subnet-mode=custom \
    --bgp-routing-mode=regional

echo ">>> Creating Subnet: $SUBNET_NAME..."
gcloud compute networks subnets create "$SUBNET_NAME" \
    --network="$VPC_NAME" \
    --region="$REGION" \
    --range="$SUBNET_RANGE" \
    --enable-private-ip-google-access # Good practice for accessing GCP APIs privately

# 3. Configure Private Service Access (PSA)
# This allocates an IP range for Google-managed services (like Cloud SQL) to use.
echo ">>> Allocating IP range for Private Service Access..."
gcloud compute addresses create google-managed-services-"$VPC_NAME" \
    --global \
    --purpose=VPC_PEERING \
    --addresses="$PSA_RANGE" \
    --network="$VPC_NAME"

echo ">>> Creating VPC Peering connection to Service Networking..."
gcloud services vpc-peerings connect \
    --service=servicenetworking.googleapis.com \
    --ranges=google-managed-services-"$VPC_NAME" \
    --network="$VPC_NAME" \
    --project="$PROJECT_ID"

# CRITICAL WAIT: 
# Creating the peering connection returns quickly, but the route propagation 
# to the Service Networking control plane can take a moment. 
# Attempting to create a Private IP Cloud SQL instance immediately often fails 
# with "Invalid Request" or networking errors.
echo ">>> Waiting 30 seconds for VPC Peering routes to propagate..."
sleep 30

# 4. Create Cloud SQL Instance
# - db-custom-1-3840: 1 vCPU, 3.75GB RAM
# - availability-type REGIONAL: Enables High Availability (Standby zone)
# - no-assign-ip: Disables Public IP
echo ">>> Creating Cloud SQL Instance: $DB_INSTANCE_NAME (This may take 5-10 minutes)..."
gcloud sql instances create "$DB_INSTANCE_NAME" \
    --project="$PROJECT_ID" \
    --region="$REGION" \
    --database-version=POSTGRES_14 \
    --tier=db-custom-1-3840 \
    --network="$VPC_NAME" \
    --no-assign-ip \
    --availability-type=REGIONAL \
    --storage-auto-increase

# 5. Create Database and User
echo ">>> Creating Database 'lakefs'..."
gcloud sql databases create lakefs \
    --instance="$DB_INSTANCE_NAME"

echo ">>> Creating Database User 'lakefs'..."
# We create the user with a temporary password placeholder. 
# In Part 2, we will rotate this using Secret Manager.
gcloud sql users create lakefs \
    --instance="$DB_INSTANCE_NAME" \
    --password="temporary-password-to-be-changed"

echo ">>> Part 1 Complete. Foundation ready."

# --- CONFIGURATION (Cont.) ---
# Ensure PROJECT_ID and REGION are set from Part 1
export BUCKET_NAME="lakefs-data-${PROJECT_ID}" # Defining a unique bucket name
export SA_VM_NAME="lakefs-sa"
export SA_SIGNER_NAME="lakefs-signer-sa"

# Service Account Emails (constructed)
export SA_VM_EMAIL="${SA_VM_NAME}@${PROJECT_ID}.iam.gserviceaccount.com"
export SA_SIGNER_EMAIL="${SA_SIGNER_NAME}@${PROJECT_ID}.iam.gserviceaccount.com"

echo ">>> Starting Part 2: Identity & Secrets setup..."

# 1. Create the GCS Bucket
# (Added for safety: The IAM commands below will fail if the bucket doesn't exist)
if ! gcloud storage buckets describe "gs://${BUCKET_NAME}" &>/dev/null; then
    echo ">>> Creating Bucket gs://${BUCKET_NAME}..."
    gcloud storage buckets create "gs://${BUCKET_NAME}" \
        --project="$PROJECT_ID" \
        --location="$REGION" \
        --uniform-bucket-level-access
else
    echo ">>> Bucket gs://${BUCKET_NAME} already exists."
fi

# 2. Create Service Accounts
echo ">>> Creating Service Account: ${SA_VM_NAME}..."
gcloud iam service-accounts create "$SA_VM_NAME" \
    --display-name="LakeFS VM Identity" \
    --project="$PROJECT_ID" || echo "SA ${SA_VM_NAME} might already exist."

echo ">>> Creating Service Account: ${SA_SIGNER_NAME}..."
gcloud iam service-accounts create "$SA_SIGNER_NAME" \
    --display-name="LakeFS GCS Signer" \
    --project="$PROJECT_ID" || echo "SA ${SA_SIGNER_NAME} might already exist."

# 3. Handle Signer Key (Generate -> Secret Manager -> Delete)
echo ">>> Generating JSON Key for ${SA_SIGNER_NAME}..."
gcloud iam service-accounts keys create lakefs-signer.json \
    --iam-account="$SA_SIGNER_EMAIL" \
    --project="$PROJECT_ID"

echo ">>> Creating Secret: lakefs-signer-key..."
# Try to create the secret container; ignore error if it already exists
gcloud secrets create lakefs-signer-key \
    --replication-policy="automatic" \
    --project="$PROJECT_ID" || echo "Secret container lakefs-signer-key exists."

echo ">>> Uploading JSON key to Secret Manager..."
gcloud secrets versions add lakefs-signer-key \
    --data-file="lakefs-signer.json" \
    --project="$PROJECT_ID"

echo ">>> Deleting local JSON key file..."
rm lakefs-signer.json

# 4. Handle DB Password (Generate -> Secret Manager -> SQL Update)
echo ">>> Generating random database password..."
# Generate 24 bytes of random data, base64 encoded
DB_PASSWORD=$(openssl rand -base64 24)

echo ">>> Creating Secret: lakefs-db-password..."
gcloud secrets create lakefs-db-password \
    --replication-policy="automatic" \
    --project="$PROJECT_ID" || echo "Secret container lakefs-db-password exists."

echo ">>> Uploading password to Secret Manager..."
# Use printf to avoid trailing newline
printf "%s" "$DB_PASSWORD" | gcloud secrets versions add lakefs-db-password \
    --data-file=- \
    --project="$PROJECT_ID"

echo ">>> Updating Cloud SQL user 'lakefs' with new password..."
gcloud sql users set-password lakefs \
    --instance="$DB_INSTANCE_NAME" \
    --password="$DB_PASSWORD" \
    --project="$PROJECT_ID"

# 5. Grant IAM Roles
echo ">>> Granting IAM Roles..."

# A. Allow VM Identity (lakefs-sa) to read secrets
gcloud projects add-iam-policy-binding "$PROJECT_ID" \
    --member="serviceAccount:${SA_VM_EMAIL}" \
    --role="roles/secretmanager.secretAccessor" \
    --condition=None

# B. Allow VM Identity to Admin the Bucket (for general operations)
gcloud storage buckets add-iam-policy-binding "gs://${BUCKET_NAME}" \
    --member="serviceAccount:${SA_VM_EMAIL}" \
    --role="roles/storage.objectAdmin"

# C. CRITICAL: Allow Signer Identity (lakefs-signer-sa) to Admin the Bucket
# Even though the VM uses the key, GCS validates the permission of the account 
# associated with the key when a pre-signed URL is used.
gcloud storage buckets add-iam-policy-binding "gs://${BUCKET_NAME}" \
    --member="serviceAccount:${SA_SIGNER_EMAIL}" \
    --role="roles/storage.objectAdmin"

echo ">>> Part 2 Complete. Identities and Secrets ready."

# --- CONFIGURATION (Cont.) ---
export BUILDER_VM_NAME="lakefs-builder-temp"
export IMAGE_NAME="lakefs-v1-image"
export ZONE="${REGION}-a"  # We just need one zone for the builder
export LAKEFS_VERSION="1.4.1"

echo ">>> Starting Part 3A: Building Golden Image..."

# 1. Check if Image Already Exists
if gcloud compute images describe "$IMAGE_NAME" --project="$PROJECT_ID" &>/dev/null; then
    echo ">>> Image $IMAGE_NAME already exists. Skipping build."
    # We exit here or skip to the next part? 
    # For this script flow, we'll mark it as done and let you decide if you want to rebuild.
    echo ">>> If you need to rebuild, delete the image: gcloud compute images delete $IMAGE_NAME"
else

    # 2. Define Build Script
    # This runs inside the temporary VM to prep the OS.
    cat <<EOF > build-script.sh
#!/bin/bash
set -e

echo ">>> [BUILD] Starting installation..."

# A. Install Dependencies
apt-get update
apt-get install -y curl

# B. Install LakeFS
echo ">>> [BUILD] Downloading LakeFS v${LAKEFS_VERSION}..."
curl -L https://github.com/treeverse/lakefs/releases/download/v${LAKEFS_VERSION}/lakefs_${LAKEFS_VERSION}_Linux_x86_64.tar.gz -o lakefs.tar.gz
tar -xzf lakefs.tar.gz
mv lakefs /usr/local/bin/
chmod +x /usr/local/bin/lakefs
rm lakefs.tar.gz

# C. Setup User and Directories
echo ">>> [BUILD] Setting up user and directories..."
useradd -m -s /bin/bash lakefs
mkdir -p /etc/lakefs /var/lib/lakefs
chown -R lakefs:lakefs /etc/lakefs /var/lib/lakefs

# D. Setup Systemd Service (Disabled)
# We create the definition, but do NOT enable/start it.
# It will be enabled in the deploy phase after config.yaml is written.
cat <<SERVICE > /etc/systemd/system/lakefs.service
[Unit]
Description=LakeFS Data Lake
After=network.target

[Service]
User=lakefs
Group=lakefs
ExecStart=/usr/local/bin/lakefs run --config /etc/lakefs/config.yaml
Restart=on-failure
LimitNOFILE=65536

[Install]
WantedBy=multi-user.target
SERVICE

echo ">>> [BUILD] Installation complete."
# CRITICAL: This string triggers the builder loop to finish
echo "BUILD_COMPLETE"
EOF

    # 3. Launch Builder VM
    # We attach a public IP (default) so it can reach GitHub.
    echo ">>> Launching Builder VM ($BUILDER_VM_NAME)..."
    gcloud compute instances create "$BUILDER_VM_NAME" \
        --project="$PROJECT_ID" \
        --zone="$ZONE" \
        --machine-type="t2d-standard-1" \
        --image-family="ubuntu-2204-lts" \
        --image-project="ubuntu-os-cloud" \
        --network="$VPC_NAME" \
        --subnet="$SUBNET_NAME" \
        --metadata-from-file=startup-script=build-script.sh \
        --scopes="cloud-platform"

    # 4. Wait for Build to Complete
    echo ">>> Waiting for build script to finish (Polling serial port)..."
    
    start_time=$(date +%s)
    timeout=600 # 10 minutes max wait
    
    while true; do
        # Check for timeout
        current_time=$(date +%s)
        elapsed=$((current_time - start_time))
        if [ "$elapsed" -ge "$timeout" ]; then
            echo "ERROR: Build timed out after $timeout seconds."
            exit 1
        fi

        # Fetch serial output
        # We suppress errors in case the API is momentarily unavailable
        output=$(gcloud compute instances get-serial-port-output "$BUILDER_VM_NAME" --zone="$ZONE" 2>/dev/null)
        
        if echo "$output" | grep -q "BUILD_COMPLETE"; then
            echo ">>> Build signal received!"
            break
        fi
        
        echo -n "."
        sleep 10
    done
    echo "" # Newline

    # 5. Stop VM and Create Image
    echo ">>> Stopping Builder VM..."
    gcloud compute instances stop "$BUILDER_VM_NAME" --zone="$ZONE"

    echo ">>> Creating Custom Image: $IMAGE_NAME..."
    gcloud compute images create "$IMAGE_NAME" \
        --project="$PROJECT_ID" \
        --source-disk="$BUILDER_VM_NAME" \
        --source-disk-zone="$ZONE" \
        --family="lakefs-server" \
        --description="LakeFS base image with binary v${LAKEFS_VERSION}"

    # 6. Cleanup
    echo ">>> Deleting Builder VM..."
    gcloud compute instances delete "$BUILDER_VM_NAME" --zone="$ZONE" --quiet
    rm build-script.sh

    echo ">>> Part 3A Complete. Image '$IMAGE_NAME' is ready."
fi

# --- CONFIGURATION (Cont.) ---
# Ensure these variables match previous parts
export TEMPLATE_NAME="lakefs-template-v1"
export MIG_NAME="lakefs-mig-regional"
export IMAGE_NAME="lakefs-v1-image"

echo ">>> Starting Part 3B: Deploying Runtime Infrastructure..."

# 1. Retrieve Cloud SQL Private IP (Again, for safety)
echo ">>> Fetching Cloud SQL Private IP..."
DB_IP=$(gcloud sql instances describe "$DB_INSTANCE_NAME" \
    --format="value(ipAddresses[0].ipAddress)")

if [[ -z "$DB_IP" ]]; then
    echo "ERROR: Could not retrieve Cloud SQL IP. Is the instance ready?"
    exit 1
fi

# 2. Define Runtime Startup Script
# This script runs on every boot of a production node.
cat <<EOF > runtime-startup.sh
#!/bin/bash
set -e

# A. Fetch Secrets
# We fetch the JSON key directly into the config directory
gcloud secrets versions access latest --secret="lakefs-signer-key" > /etc/lakefs/signer-key.json
chown lakefs:lakefs /etc/lakefs/signer-key.json
chmod 600 /etc/lakefs/signer-key.json

# Fetch DB Password into a variable
DB_PASSWORD=\$(gcloud secrets versions access latest --secret="lakefs-db-password")

# B. Get Metadata Variables
# The instance template passes these in.
DB_HOST=\$(curl -s "http://metadata.google.internal/computeMetadata/v1/instance/attributes/db-ip" -H "Metadata-Flavor: Google")
BUCKET=\$(curl -s "http://metadata.google.internal/computeMetadata/v1/instance/attributes/bucket-name" -H "Metadata-Flavor: Google")

# C. Write config.yaml
# Dynamic configuration based on the environment we are waking up in.
cat <<CONFIG > /etc/lakefs/config.yaml
database:
  type: "postgres"
  postgres:
    connection_string: "postgres://lakefs:\${DB_PASSWORD}@\${DB_HOST}:5432/lakefs"

blockstore:
  type: "gs"
  gs:
    credentials_json: "/etc/lakefs/signer-key.json"

storage:
  type: "gs"
  
installation:
  user_data_dir: "/var/lib/lakefs"

listen_address: "0.0.0.0:8000"

logging:
  level: "INFO"
CONFIG

chown lakefs:lakefs /etc/lakefs/config.yaml

# D. Start Service
# Since the unit file exists (from the image), we just enable and start it.
systemctl enable lakefs
systemctl start lakefs

echo ">>> Runtime configuration complete. LakeFS started."
EOF

# 3. Create Health Check
echo ">>> Creating Health Check..."
gcloud compute health-checks create http lakefs-health-check \
    --port=8000 \
    --request-path="/_health" \
    --check-interval=5s \
    --timeout=5s \
    --unhealthy-threshold=2 \
    --healthy-threshold=2 \
    --region="$REGION" || echo "Health check might already exist."

# 4. Create Instance Template (Using Golden Image)
echo ">>> Creating Instance Template ($TEMPLATE_NAME)..."
gcloud compute instance-templates create "$TEMPLATE_NAME" \
    --project="$PROJECT_ID" \
    --region="$REGION" \
    --image="$IMAGE_NAME" \
    --machine-type="t2d-standard-1" \
    --boot-disk-type=pd-balanced \
    --network-interface=network="$VPC_NAME",subnet="$SUBNET_NAME",no-address \
    --tags="lakefs-server" \
    --service-account="$SA_VM_EMAIL" \
    --scopes="https://www.googleapis.com/auth/cloud-platform" \
    --metadata-from-file=startup-script=runtime-startup.sh \
    --metadata=db-ip="$DB_IP",bucket-name="$BUCKET_NAME"

# 5. Create Regional MIG
echo ">>> Creating Regional MIG ($MIG_NAME)..."
gcloud compute instance-groups managed create "$MIG_NAME" \
    --project="$PROJECT_ID" \
    --region="$REGION" \
    --base-instance-name="lakefs" \
    --template="$TEMPLATE_NAME" \
    --size=2 \
    --zones="${REGION}-a,${REGION}-b,${REGION}-c" \
    --target-distribution-shape=EVEN

# 6. Configure Autohealing & Named Ports
echo ">>> Configuring Autohealing and Named Ports..."
gcloud compute instance-groups managed set-named-ports "$MIG_NAME" \
    --named-ports="http:8000" \
    --region="$REGION"

gcloud compute instance-groups managed update "$MIG_NAME" \
    --region="$REGION" \
    --health-check="lakefs-health-check" \
    --initial-delay=60 \
    # Reduced delay (60s) because startup is now very fast

# Cleanup local script
rm runtime-startup.sh

echo ">>> Part 3B Complete. Production Fleet Launched."

# --- CONFIGURATION (Cont.) ---
# REPLACE THIS with your actual domain
export DOMAIN_NAME="lakefs.yourcompany.com" 

export IP_NAME="lakefs-global-ip"
export CERT_NAME="lakefs-ssl-cert"
export LB_PREFIX="lakefs-lb"

echo ">>> Starting Part 4: Load Balancing & Ingress..."

# 1. Reserve Global Static IP
echo ">>> Reserving Global Static IP ($IP_NAME)..."
# Check if exists first to avoid error
if ! gcloud compute addresses describe "$IP_NAME" --global --project="$PROJECT_ID" &>/dev/null; then
    gcloud compute addresses create "$IP_NAME" \
        --global \
        --ip-version=IPV4 \
        --project="$PROJECT_ID"
fi

# Retrieve the actual IP address
LB_IP_ADDRESS=$(gcloud compute addresses describe "$IP_NAME" \
    --global \
    --format="value(address)" \
    --project="$PROJECT_ID")

echo ">>> IP Address Reserved: $LB_IP_ADDRESS"

# 2. Create Managed SSL Certificate
echo ">>> Creating Managed SSL Certificate for $DOMAIN_NAME..."
if ! gcloud compute ssl-certificates describe "$CERT_NAME" --global --project="$PROJECT_ID" &>/dev/null; then
    gcloud compute ssl-certificates create "$CERT_NAME" \
        --domains="$DOMAIN_NAME" \
        --global \
        --project="$PROJECT_ID"
else
    echo ">>> Certificate $CERT_NAME already exists."
fi

# 3. Create Backend Service
echo ">>> Creating Backend Service..."
if ! gcloud compute backend-services describe "${LB_PREFIX}-backend" --global --project="$PROJECT_ID" &>/dev/null; then
    gcloud compute backend-services create "${LB_PREFIX}-backend" \
        --protocol=HTTP \
        --port-name=http \
        --health-checks="lakefs-health-check" \
        --global \
        --timeout=30s \
        --project="$PROJECT_ID"

    # Add the Regional MIG to this Backend Service
    echo ">>> Adding MIG ($MIG_NAME) to Backend Service..."
    gcloud compute backend-services add-backend "${LB_PREFIX}-backend" \
        --instance-group="$MIG_NAME" \
        --instance-group-region="$REGION" \
        --global \
        --balancing-mode=UTILIZATION \
        --max-utilization=0.8 \
        --project="$PROJECT_ID"
fi

# 4. Create URL Map
echo ">>> Creating URL Map..."
if ! gcloud compute url-maps describe "${LB_PREFIX}-url-map" --global --project="$PROJECT_ID" &>/dev/null; then
    gcloud compute url-maps create "${LB_PREFIX}-url-map" \
        --default-service="${LB_PREFIX}-backend" \
        --project="$PROJECT_ID"
fi

# 5. Create Target HTTPS Proxy
echo ">>> Creating Target HTTPS Proxy..."
if ! gcloud compute target-https-proxies describe "${LB_PREFIX}-https-proxy" --global --project="$PROJECT_ID" &>/dev/null; then
    gcloud compute target-https-proxies create "${LB_PREFIX}-https-proxy" \
        --url-map="${LB_PREFIX}-url-map" \
        --ssl-certificates="$CERT_NAME" \
        --project="$PROJECT_ID"
fi

# 6. Create Global Forwarding Rule
echo ">>> Creating Global Forwarding Rule..."
if ! gcloud compute forwarding-rules describe "${LB_PREFIX}-forwarding-rule" --global --project="$PROJECT_ID" &>/dev/null; then
    gcloud compute forwarding-rules create "${LB_PREFIX}-forwarding-rule" \
        --address="$LB_IP_ADDRESS" \
        --global \
        --target-https-proxy="${LB_PREFIX}-https-proxy" \
        --ports=443 \
        --project="$PROJECT_ID"
fi

# 7. Create Firewall Rule (Allow Google LB -> Instances)
# Google LBs use specific source ranges: 130.211.0.0/22 and 35.191.0.0/16
echo ">>> Creating Firewall Rule for Load Balancer Health Checks..."
if ! gcloud compute firewall-rules describe allow-lb-to-lakefs --project="$PROJECT_ID" &>/dev/null; then
    gcloud compute firewall-rules create allow-lb-to-lakefs \
        --network="$VPC_NAME" \
        --action=ALLOW \
        --direction=INGRESS \
        --source-ranges="130.211.0.0/22,35.191.0.0/16" \
        --target-tags="lakefs-server" \
        --rules=tcp:8000 \
        --project="$PROJECT_ID"
fi

# 8. Final Output
echo "----------------------------------------------------"
echo ">>> Part 4 Complete."
echo ">>> ACTION REQUIRED: Update your DNS records."
echo ">>> Create an A Record for $DOMAIN_NAME pointing to: $LB_IP_ADDRESS"
echo ">>> Note: SSL Certificate will remain in 'PROVISIONING' status until DNS propagates."
echo "----------------------------------------------------"

# --- CONFIGURATION (Cont.) ---
export GC_SA_NAME="lakefs-gc-sa"
export SCHEDULER_SA_NAME="lakefs-scheduler-sa"
export GC_JOB_NAME="lakefs-gc-daily"

# Service Account Emails
export GC_SA_EMAIL="${GC_SA_NAME}@${PROJECT_ID}.iam.gserviceaccount.com"
export SCHEDULER_SA_EMAIL="${SCHEDULER_SA_NAME}@${PROJECT_ID}.iam.gserviceaccount.com"

echo ">>> Starting Part 5: Maintenance Automation (GC)..."

# 1. Create Service Accounts
# A. The SA that runs the actual Spark Job (needs GCS access)
echo ">>> Creating GC Service Account ($GC_SA_NAME)..."
gcloud iam service-accounts create "$GC_SA_NAME" \
    --display-name="LakeFS GC Runner" \
    --project="$PROJECT_ID" || echo "SA $GC_SA_NAME exists."

# B. The SA that Cloud Scheduler uses to trigger the API (needs Dataproc access)
echo ">>> Creating Scheduler Service Account ($SCHEDULER_SA_NAME)..."
gcloud iam service-accounts create "$SCHEDULER_SA_NAME" \
    --display-name="LakeFS Scheduler Trigger" \
    --project="$PROJECT_ID" || echo "SA $SCHEDULER_SA_NAME exists."

# 2. Grant Permissions
echo ">>> Granting IAM Roles..."

# A. Give GC Runner access to the Bucket (to delete old objects)
gcloud storage buckets add-iam-policy-binding "gs://${BUCKET_NAME}" \
    --member="serviceAccount:${GC_SA_EMAIL}" \
    --role="roles/storage.objectAdmin"

# B. Give GC Runner access to run Dataproc Batches
gcloud projects add-iam-policy-binding "$PROJECT_ID" \
    --member="serviceAccount:${GC_SA_EMAIL}" \
    --role="roles/dataproc.worker" \
    --condition=None

# C. Give Scheduler SA permission to trigger Dataproc
gcloud projects add-iam-policy-binding "$PROJECT_ID" \
    --member="serviceAccount:${SCHEDULER_SA_EMAIL}" \
    --role="roles/dataproc.editor" \
    --condition=None

# D. Allow Scheduler SA to act as the GC Runner (PassRole)
gcloud iam service-accounts add-iam-policy-binding "$GC_SA_EMAIL" \
    --member="serviceAccount:${SCHEDULER_SA_EMAIL}" \
    --role="roles/iam.serviceAccountUser"

# 3. Define the Batch JSON Payload
# We construct the JSON body for the Dataproc API call.
# NOTE: This uses the official treeverse/lakefs-spark-gc image.
# IMPORTANT: You must replace the KEY/SECRET placeholders after generating keys in the UI.

cat <<EOF > batch-request.json
{
  "sparkBatch": {
    "mainClass": "io.treeverse.gc.GarbageCollectorDriver",
    "args": [
      "run",
      "--repo", "example-repo",
      "--region", "${REGION}",
      "--mark", "gs://${BUCKET_NAME}/_lakefs/gc/mark",
      "--sweep", "gs://${BUCKET_NAME}/_lakefs/gc/sweep"
    ],
    "runtimeConfig": {
        "containerImage": "treeverse/lakefs-spark-gc:latest",
        "properties": {
            "spark.hadoop.lakefs.api.url": "https://${DOMAIN_NAME}/api/v1",
            "spark.hadoop.lakefs.api.access_key": "REPLACE_WITH_ACCESS_KEY",
            "spark.hadoop.lakefs.api.secret_key": "REPLACE_WITH_SECRET_KEY"
        }
    }
  },
  "environmentConfig": {
    "executionConfig": {
      "serviceAccount": "${GC_SA_EMAIL}",
      "subnetworkUri": "regions/${REGION}/subnetworks/${SUBNET_NAME}"
    }
  }
}
EOF

# 4. Create Cloud Scheduler Job
echo ">>> Creating Cloud Scheduler Job ($GC_JOB_NAME)..."

# We use the raw REST API target because 'gcloud scheduler jobs create' 
# does not natively support triggering Dataproc Batches directly yet.
if ! gcloud scheduler jobs describe "$GC_JOB_NAME" --location="$REGION" --project="$PROJECT_ID" &>/dev/null; then
    gcloud scheduler jobs create http "$GC_JOB_NAME" \
        --location="$REGION" \
        --schedule="0 3 * * *" \
        --time-zone="Etc/UTC" \
        --uri="https://dataproc.googleapis.com/v1/projects/${PROJECT_ID}/locations/${REGION}/batches" \
        --http-method=POST \
        --oauth-service-account-email="$SCHEDULER_SA_EMAIL" \
        --headers="Content-Type=application/json" \
        --message-body-from-file=batch-request.json \
        --project="$PROJECT_ID"
else
    echo ">>> Job $GC_JOB_NAME already exists."
fi

# Cleanup
rm batch-request.json

echo "----------------------------------------------------"
echo ">>> COMPLETE. LakeFS Infrastructure is deployed."
echo "----------------------------------------------------"
echo ">>> NEXT STEPS:"
echo "1. Wait for DNS to propagate."
echo "2. Visit http://${DOMAIN_NAME}/setup to create your Admin User."
echo "3. Generate an Access Key/Secret for the Garbage Collector."
echo "4. Update the Cloud Scheduler job with these credentials:"
echo "   gcloud scheduler jobs update http $GC_JOB_NAME --location=$REGION --message-body=..."
echo "----------------------------------------------------"
