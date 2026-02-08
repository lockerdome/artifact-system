#!/bin/bash
set -e  # Exit immediately if a command exits with a non-zero status

cleanup() {
    rm -f lakefs-signer.json build-script.sh runtime-startup.sh batch-request.json admin-creds.json
    echo ">>> Cleanup complete."
}
trap cleanup EXIT

# --- CONFIGURATION ---
export PROJECT_ID="your-project-id"  # <--- REPLACE THIS
export LAKEFS_VERSION="1.76.0"
export REGION="us-central1"
export VPC_NAME="lakefs-vpc"
export SUBNET_NAME="lakefs-subnet-${REGION}"
export PROXY_SUBNET_NAME="lakefs-proxy-subnet-${REGION}"
export SUBNET_RANGE="10.0.0.0/20"    # Range for your VMs
export PSA_RANGE="10.100.0.0/16"     # Range for Cloud SQL (Service Peering)
export LB_RANGE="10.129.0.0/23"
export DB_INSTANCE_NAME="lakefs-db"
export LB_PREFIX="lakefs-lb"
export ACCESS_KEY_ID="e4f3c46f902cce3b13da679b"

# Set the project context
gcloud config set project "$PROJECT_ID"

echo ">>> Starting Part 1: Foundation setup for Project: $PROJECT_ID in $REGION"

# 1. Enable APIs
echo ">>> Enabling required APIs..."
gcloud services enable \
    compute.googleapis.com \
    sqladmin.googleapis.com \
    servicenetworking.googleapis.com \
    secretmanager.googleapis.com \
    dataproc.googleapis.com \
    iap.googleapis.com \
    cloudscheduler.googleapis.com

# 2. Create VPC and Subnet
if ! gcloud compute networks describe "$VPC_NAME" --project="$PROJECT_ID" &>/dev/null; then
    echo ">>> Creating VPC: $VPC_NAME..."
    gcloud compute networks create "$VPC_NAME" \
        --subnet-mode=custom \
        --bgp-routing-mode=regional \
        --project="$PROJECT_ID"
else
    echo ">>> VPC $VPC_NAME already exists."
fi

if ! gcloud compute routers describe lakefs-router --region="$REGION" --project="$PROJECT_ID" &>/dev/null; then
    echo ">>> Creating Cloud Router..."
    gcloud compute routers create lakefs-router \
        --network="$VPC_NAME" \
        --region="$REGION" \
        --project="$PROJECT_ID"
else
    echo ">>> Cloud Router lakefs-router already exists."
fi

if ! gcloud compute routers nats describe lakefs-nat --region="$REGION" --router=lakefs-router --project="$PROJECT_ID" &>/dev/null; then
    echo ">>> Creating Cloud NAT..."
    gcloud compute routers nats create lakefs-nat \
        --router=lakefs-router \
        --region="$REGION" \
        --auto-allocate-nat-external-ips \
        --nat-all-subnet-ip-ranges \
        --project="$PROJECT_ID"
else
    echo ">>> Cloud NAT lakefs-nat already exists."
fi

if ! gcloud compute networks subnets describe "$SUBNET_NAME" --region="$REGION" --project="$PROJECT_ID" &>/dev/null; then
    echo ">>> Creating Subnet: $SUBNET_NAME..."
    gcloud compute networks subnets create "$SUBNET_NAME" \
        --network="$VPC_NAME" \
        --region="$REGION" \
        --range="$SUBNET_RANGE" \
        --enable-private-ip-google-access \
        --project="$PROJECT_ID"
else
    echo ">>> Subnet $SUBNET_NAME already exists."
fi

# 3. Configure Private Service Access (PSA)
if ! gcloud compute addresses describe google-managed-services-"$VPC_NAME" --global --project="$PROJECT_ID" &>/dev/null; then
    echo ">>> Allocating IP range for Private Service Access..."
    gcloud compute addresses create google-managed-services-"$VPC_NAME" \
        --global \
        --purpose=VPC_PEERING \
        --addresses="${PSA_RANGE%/*}" \
        --prefix-length="${PSA_RANGE#*/}" \
        --network="$VPC_NAME" \
        --project="$PROJECT_ID"
else
    echo ">>> PSA Address Range already allocated."
fi

# Check if peering exists by looking for the service networking connection
PEERING_EXISTS=$(gcloud compute networks peerings list --network="$VPC_NAME" --project="$PROJECT_ID" --format="value(name)" | grep "servicenetworking-googleapis-com" || true)

if [[ -z "$PEERING_EXISTS" ]]; then
    echo ">>> Creating VPC Peering connection to Service Networking..."
    gcloud services vpc-peerings connect \
        --service=servicenetworking.googleapis.com \
        --ranges=google-managed-services-"$VPC_NAME" \
        --network="$VPC_NAME" \
        --project="$PROJECT_ID"
    
    echo ">>> Waiting 30 seconds for VPC Peering routes to propagate..."
    sleep 30
else
    echo ">>> VPC Peering connection already exists."
fi

# 4. Create Cloud SQL Instance
if ! gcloud sql instances describe "$DB_INSTANCE_NAME" --project="$PROJECT_ID" &>/dev/null; then
    echo ">>> Creating Cloud SQL Instance: $DB_INSTANCE_NAME..."
    gcloud sql instances create "$DB_INSTANCE_NAME" \
        --project="$PROJECT_ID" \
        --region="$REGION" \
        --database-version=POSTGRES_14 \
        --tier=db-custom-1-3840 \
        --network="$VPC_NAME" \
        --no-assign-ip \
        --availability-type=REGIONAL \
        --storage-auto-increase
else
    echo ">>> Cloud SQL Instance $DB_INSTANCE_NAME already exists."
fi

# 5. Create Database and User
if ! gcloud sql databases describe lakefs --instance="$DB_INSTANCE_NAME" --project="$PROJECT_ID" &>/dev/null; then
    echo ">>> Creating Database 'lakefs'..."
    gcloud sql databases create lakefs --instance="$DB_INSTANCE_NAME"
else
    echo ">>> Database 'lakefs' already exists."
fi

# Check if user exists (filtering correctly to avoid error on grep)
USER_EXISTS=$(gcloud sql users list --instance="$DB_INSTANCE_NAME" --project="$PROJECT_ID" --format="value(name)" | grep "^lakefs$" || true)
if [[ -z "$USER_EXISTS" ]]; then
    echo ">>> Creating Database User 'lakefs'..."
    gcloud sql users create lakefs \
        --instance="$DB_INSTANCE_NAME" \
        --password="temporary-password-to-be-changed" \
        --project="$PROJECT_ID"
else
    echo ">>> Database User 'lakefs' already exists."
fi

echo ">>> Part 1 Complete. Foundation ready."

# --- CONFIGURATION (Cont.) ---
export BUCKET_NAME="lakefs-data-${PROJECT_ID}"
export SA_VM_NAME="lakefs-sa"
export SA_SIGNER_NAME="lakefs-signer-sa"
export SA_VM_EMAIL="${SA_VM_NAME}@${PROJECT_ID}.iam.gserviceaccount.com"
export SA_SIGNER_EMAIL="${SA_SIGNER_NAME}@${PROJECT_ID}.iam.gserviceaccount.com"

echo ">>> Starting Part 2: Identity & Secrets setup..."

# 1. Create the GCS Bucket
if ! gcloud storage buckets describe "gs://${BUCKET_NAME}" --project="$PROJECT_ID" &>/dev/null; then
    echo ">>> Creating Bucket gs://${BUCKET_NAME}..."
    gcloud storage buckets create "gs://${BUCKET_NAME}" \
        --project="$PROJECT_ID" \
        --location="$REGION" \
        --uniform-bucket-level-access
else
    echo ">>> Bucket gs://${BUCKET_NAME} already exists."
fi

# 2. Create Service Accounts
if ! gcloud iam service-accounts describe "$SA_VM_EMAIL" --project="$PROJECT_ID" &>/dev/null; then
    echo ">>> Creating Service Account: ${SA_VM_NAME}..."
    gcloud iam service-accounts create "$SA_VM_NAME" \
        --display-name="LakeFS VM Identity" \
        --project="$PROJECT_ID"
else
    echo ">>> SA ${SA_VM_NAME} already exists."
fi

if ! gcloud iam service-accounts describe "$SA_SIGNER_EMAIL" --project="$PROJECT_ID" &>/dev/null; then
    echo ">>> Creating Service Account: ${SA_SIGNER_NAME}..."
    gcloud iam service-accounts create "$SA_SIGNER_NAME" \
        --display-name="LakeFS GCS Signer" \
        --project="$PROJECT_ID"
else
    echo ">>> SA ${SA_SIGNER_NAME} already exists."
fi

echo ">>> Waiting 20s for Service Account propagation..."
sleep 20

# 3. Handle Signer Key
if ! gcloud secrets describe lakefs-signer-key --project="$PROJECT_ID" &>/dev/null; then
    echo ">>> Creating Signer Key..."
    gcloud iam service-accounts keys create lakefs-signer.json \
        --iam-account="$SA_SIGNER_EMAIL" \
        --project="$PROJECT_ID"

    echo ">>> Creating Secret: lakefs-signer-key..."
    gcloud secrets create lakefs-signer-key \
        --replication-policy="automatic" \
        --project="$PROJECT_ID"

    echo ">>> Uploading key version..."
    gcloud secrets versions add lakefs-signer-key \
        --data-file="lakefs-signer.json" \
        --project="$PROJECT_ID"
    rm lakefs-signer.json
else
    echo ">>> Secret lakefs-signer-key already exists. Reusing existing key."
fi

# 4. Handle DB Password
# We check if password secret exists. If it does, we assume it's set to avoid resetting DB password causing downtime.
if ! gcloud secrets describe lakefs-db-password --project="$PROJECT_ID" &>/dev/null; then
    echo ">>> Generating random database password..."
    DB_PASSWORD=$(openssl rand -hex 24)

    echo ">>> Creating Secret: lakefs-db-password..."
    gcloud secrets create lakefs-db-password \
        --replication-policy="automatic" \
        --project="$PROJECT_ID"

    printf "%s" "$DB_PASSWORD" | gcloud secrets versions add lakefs-db-password \
        --data-file=- \
        --project="$PROJECT_ID"

    echo ">>> Updating Cloud SQL user 'lakefs' with new password..."
    gcloud sql users set-password lakefs \
        --instance="$DB_INSTANCE_NAME" \
        --password="$DB_PASSWORD" \
        --project="$PROJECT_ID"
else
    echo ">>> Secret lakefs-db-password already exists."
    echo ">>> Ensuring Cloud SQL user is synced with existing secret..."
    
    # Fetch the existing password
    EXISTING_PW=$(gcloud secrets versions access latest --secret="lakefs-db-password" --project="$PROJECT_ID")
    
    # Force update the SQL user to match
    gcloud sql users set-password lakefs \
        --instance="$DB_INSTANCE_NAME" \
        --password="$EXISTING_PW" \
        --project="$PROJECT_ID"
fi

# 5. Grant IAM Roles (Idempotent by default)
echo ">>> Ensuring IAM Roles..."
gcloud projects add-iam-policy-binding "$PROJECT_ID" \
    --member="serviceAccount:${SA_VM_EMAIL}" \
    --role="roles/secretmanager.secretAccessor" \
    --condition=None >/dev/null

gcloud storage buckets add-iam-policy-binding "gs://${BUCKET_NAME}" \
    --member="serviceAccount:${SA_VM_EMAIL}" \
    --role="roles/storage.objectAdmin" >/dev/null

gcloud storage buckets add-iam-policy-binding "gs://${BUCKET_NAME}" \
    --member="serviceAccount:${SA_SIGNER_EMAIL}" \
    --role="roles/storage.objectAdmin" >/dev/null

# 6. Create authorization key
if ! gcloud secrets describe lakefs-secret-key --project="$PROJECT_ID" &>/dev/null; then
    echo ">>> Generating random secret key for LakeFS..."
    SECRET_KEY=$(openssl rand -hex 24)

    echo ">>> Creating Secret: lakefs-secret-key..."
    gcloud secrets create lakefs-secret-key \
        --replication-policy="automatic" \
        --project="$PROJECT_ID"

    printf "%s" "$SECRET_KEY" | gcloud secrets versions add lakefs-secret-key \
        --data-file=- \
        --project="$PROJECT_ID"
else
    echo ">>> Secret lakefs-secret-key already exists."
    SECRET_KEY=$(gcloud secrets versions access latest --secret="lakefs-secret-key" --project="$PROJECT_ID")
fi

# 7. Create admin secret access key
if ! gcloud secrets describe lakefs-secret-access-key --project="$PROJECT_ID" &>/dev/null; then
    echo ">>> Generating random secret key for LakeFS..."
    SECRET_ACCESS_KEY=$(openssl rand -hex 24)

    echo ">>> Creating Secret: lakefs-secret-access-key..."
    gcloud secrets create lakefs-secret-access-key \
        --replication-policy="automatic" \
        --project="$PROJECT_ID"

    printf "%s" "$SECRET_ACCESS_KEY" | gcloud secrets versions add lakefs-secret-access-key \
        --data-file=- \
        --project="$PROJECT_ID"
else
    echo ">>> Secret lakefs-secret-access-key already exists."
    SECRET_ACCESS_KEY=$(gcloud secrets versions access latest --secret="lakefs-secret-access-key" --project="$PROJECT_ID")
fi

echo ">>> Part 2 Complete. Identities and Secrets ready."

# --- CONFIGURATION (Cont.) ---
export BUILDER_VM_NAME="lakefs-builder-temp"
export IMAGE_NAME="lakefs-v${LAKEFS_VERSION//./-}-image"
export ZONE="${REGION}-a"

echo ">>> Starting Part 3A: Building Golden Image..."

if gcloud compute images describe "$IMAGE_NAME" --project="$PROJECT_ID" &>/dev/null; then
    echo ">>> Image $IMAGE_NAME already exists. Skipping build."
else
    # CLEANUP STALE BUILDER IF EXISTS
    if gcloud compute instances describe "$BUILDER_VM_NAME" --zone="$ZONE" --project="$PROJECT_ID" &>/dev/null; then
        echo ">>> Found stale Builder VM. Deleting..."
        gcloud compute instances delete "$BUILDER_VM_NAME" --zone="$ZONE" --quiet
    fi

    # 2. Define Build Script
    cat <<EOF > build-script.sh
#!/bin/bash
set -e
echo ">>> [BUILD] Starting installation..."
apt-get update
apt-get install -y curl ca-certificates gnupg apt-transport-https
curl -fsSL https://packages.cloud.google.com/apt/doc/apt-key.gpg | gpg --dearmor -o /usr/share/keyrings/cloud.google.gpg
echo "deb [signed-by=/usr/share/keyrings/cloud.google.gpg] https://packages.cloud.google.com/apt cloud-sdk main" > /etc/apt/sources.list.d/google-cloud-sdk.list
apt-get update
apt-get install -y google-cloud-cli
echo ">>> [BUILD] Downloading LakeFS v${LAKEFS_VERSION}..."
curl -L --fail https://github.com/treeverse/lakefs/releases/download/v${LAKEFS_VERSION}/lakefs_${LAKEFS_VERSION}_Linux_x86_64.tar.gz -o lakefs.tar.gz
tar -xzf lakefs.tar.gz
mv lakefs /usr/local/bin/
chmod +x /usr/local/bin/lakefs
rm lakefs.tar.gz
echo ">>> [BUILD] Setting up user..."
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

    echo ">>> Launching Builder VM ($BUILDER_VM_NAME)..."
    gcloud compute instances create "$BUILDER_VM_NAME" \
        --project="$PROJECT_ID" \
        --zone="$ZONE" \
        --machine-type="t2d-standard-1" \
        --image-family="ubuntu-2204-lts" \
        --image-project="ubuntu-os-cloud" \
        --network="$VPC_NAME" \
        --subnet="$SUBNET_NAME" \
        --no-address \
        --metadata-from-file=startup-script=build-script.sh \
        --metadata=vmDnsSetting=ZonalOnly \
        --scopes="cloud-platform"

    echo ">>> Waiting for build script to finish (Polling serial port)..."

        start_time=$(date +%s)
        timeout=600 # 10 minutes max wait

        while true; do
            current_time=$(date +%s)
            if [ $((current_time - start_time)) -ge "$timeout" ]; then
                echo "ERROR: Build timed out."
                exit 1
            fi

            # Fetch serial output; ignore errors if API is busy
            output=$(gcloud compute instances get-serial-port-output "$BUILDER_VM_NAME" --zone="$ZONE" 2>/dev/null)

            if echo "$output" | grep -q "BUILD_COMPLETE"; then
                echo ">>> Build signal received!"
                break
            fi

            echo -n "."
            sleep 10
        done
        echo ""

    echo ">>> Stopping Builder VM..."
    gcloud compute instances stop "$BUILDER_VM_NAME" --zone="$ZONE" --project="$PROJECT_ID"

    echo ">>> Creating Custom Image: $IMAGE_NAME..."
    gcloud compute images create "$IMAGE_NAME" \
        --project="$PROJECT_ID" \
        --source-disk="$BUILDER_VM_NAME" \
        --source-disk-zone="$ZONE" \
        --family="lakefs-server"

    echo ">>> Deleting Builder VM..."
    gcloud compute instances delete "$BUILDER_VM_NAME" --zone="$ZONE" --project="$PROJECT_ID" --quiet
    rm build-script.sh
fi

echo ">>> Part 3A Complete."

# --- CONFIGURATION (Cont.) ---
export TEMPLATE_NAME="lakefs-template-v1"
export MIG_NAME="lakefs-mig-regional"

echo ">>> Starting Part 3B: Deploying Runtime Infrastructure..."

echo ">>> Fetching Cloud SQL Private IP..."
DB_IP=""
MAX_RETRIES=10
COUNT=0
while [ -z "$DB_IP" ] && [ $COUNT -lt $MAX_RETRIES ]; do
    DB_IP=$(gcloud sql instances describe "$DB_INSTANCE_NAME" --project="$PROJECT_ID" --format="json" 2>/dev/null | python3 -c "import sys, json; print(next((i['ipAddress'] for i in json.load(sys.stdin).get('ipAddresses', []) if i['type'] == 'PRIVATE'), ''))")
    if [ -n "$DB_IP" ]; then
        echo "    Found DB IP: $DB_IP"
        break
    fi
    echo "    Waiting for DB IP to be available... ($COUNT/$MAX_RETRIES)"
    sleep 10
    COUNT=$((COUNT+1))
done

if [[ -z "$DB_IP" ]]; then
    echo "ERROR: Could not retrieve Cloud SQL Private IP. Please verify the instance '$DB_INSTANCE_NAME' has a Private IP."
    exit 1
fi

# Create Startup Script with DB MIGRATION & RETRIES
cat <<EOF > runtime-startup.sh
#!/bin/bash
set -x  # Enable debug logging (Check /var/log/syslog)

# Robustly fetch metadata with retries
fetch_metadata() {
  local attr=\$1
  local val=""
  local count=0
  while [ -z "\$val" ] && [ \$count -lt 20 ]; do
    val=\$(curl -s "http://metadata.google.internal/computeMetadata/v1/instance/attributes/\$attr" -H "Metadata-Flavor: Google")
    if [ -z "\$val" ]; then sleep 3; fi
    count=\$((count+1))
  done
  echo "\$val"
}

# 1. Fetch Secrets (with Retries)
echo ">>> Fetching secrets..."
RETRIES=0
while [ \$RETRIES -lt 20 ]; do
    if gcloud secrets versions access latest --secret="lakefs-signer-key" > /etc/lakefs/signer-key.json 2>/dev/null; then
        if [ -s /etc/lakefs/signer-key.json ]; then break; fi
    fi
    echo "Waiting for Secret Manager (Signer Key)..."
    sleep 3
    RETRIES=\$((RETRIES+1))
done

RETRIES=0
while [ \$RETRIES -lt 20 ]; do
    SECRET_KEY=\$(gcloud secrets versions access latest --secret="lakefs-secret-key" 2>/dev/null)
    if [ -n "\$SECRET_KEY" ]; then break; fi
    echo "Waiting for Secret Manager (Auth Key)..."
    sleep 3
    RETRIES=\$((RETRIES+1))
done

RETRIES=0
while [ \$RETRIES -lt 20 ]; do
    SECRET_ACCESS_KEY=\$(gcloud secrets versions access latest --secret="lakefs-secret-access-key" 2>/dev/null)
    if [ -n "\$SECRET_ACCESS_KEY" ]; then break; fi
    echo "Waiting for Secret Manager (Auth Key)..."
    sleep 3
    RETRIES=\$((RETRIES+1))
done

RETRIES=0
while [ \$RETRIES -lt 20 ]; do
    DB_PASSWORD=\$(gcloud secrets versions access latest --secret="lakefs-db-password" 2>/dev/null)
    if [ -n "\$DB_PASSWORD" ]; then break; fi
    echo "Waiting for Secret Manager (Pass)..."
    sleep 3
    RETRIES=\$((RETRIES+1))
done

if [ -z "\$DB_PASSWORD" ]; then
    echo "ERROR: Could not fetch DB Password."
    exit 1
fi

chown lakefs:lakefs /etc/lakefs/signer-key.json
chmod 600 /etc/lakefs/signer-key.json

DB_HOST=\$(fetch_metadata "db-ip")
BUCKET=\$(fetch_metadata "bucket-name")

if [ -z "\$DB_HOST" ]; then
    echo "ERROR: Failed to fetch DB_HOST from metadata."
    exit 1
fi

cat <<CONFIG > /etc/lakefs/config.yaml
database:
  type: "postgres"
  postgres:
    connection_string: "postgres://lakefs:\${DB_PASSWORD}@\${DB_HOST}:5432/lakefs"

blockstore:
  type: "gs"
  gs:
    credentials_file: "/etc/lakefs/signer-key.json"

auth:
  encrypt:
    secret_key: "\${SECRET_KEY}"

listen_address: "0.0.0.0:8000"

logging:
  format: json
  level: WARN
  output: "-"
CONFIG

chown lakefs:lakefs /etc/lakefs/config.yaml

/usr/local/bin/lakefs --config /etc/lakefs/config.yaml setup \
  --user-name "admin" \
  --access-key-id "${ACCESS_KEY_ID}" \
  --secret-access-key "\${SECRET_ACCESS_KEY}" || true

echo ">>> Starting LakeFS..."
systemctl enable lakefs && systemctl start lakefs
EOF

# 3. Create Health Check
if ! gcloud compute health-checks describe lakefs-health-check --region="$REGION" --project="$PROJECT_ID" &>/dev/null; then
    echo ">>> Creating Health Check..."
    gcloud compute health-checks create http lakefs-health-check \
        --region="$REGION" \
        --port=8000 \
        --request-path="/_health" \
        --check-interval=5s \
        --timeout=5s \
        --unhealthy-threshold=2 \
        --healthy-threshold=2 \
        --project="$PROJECT_ID"
else
    echo ">>> Health check already exists."
fi

# 4. Create Instance Template
if ! gcloud compute instance-templates describe "$TEMPLATE_NAME" --project="$PROJECT_ID" &>/dev/null; then
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
        --metadata=db-ip="$DB_IP",bucket-name="$BUCKET_NAME",vmDnsSetting=ZonalOnly
else
    echo ">>> Instance Template $TEMPLATE_NAME already exists."
fi

# 5. Create Regional MIG
if ! gcloud compute instance-groups managed describe "$MIG_NAME" --region="$REGION" --project="$PROJECT_ID" &>/dev/null; then
    echo ">>> Creating Regional MIG ($MIG_NAME)..."
    gcloud compute instance-groups managed create "$MIG_NAME" \
        --project="$PROJECT_ID" \
        --region="$REGION" \
        --base-instance-name="lakefs" \
        --template="$TEMPLATE_NAME" \
        --size=2 \
        --zones="${REGION}-a,${REGION}-b,${REGION}-c" \
        --health-check="https://www.googleapis.com/compute/v1/projects/${PROJECT_ID}/regions/${REGION}/healthChecks/lakefs-health-check" \
        --initial-delay=300
else
    echo ">>> MIG $MIG_NAME already exists."
fi

echo ">>> Configuring Autohealing and Named Ports..."
gcloud compute instance-groups managed set-named-ports "$MIG_NAME" \
    --named-ports="http:8000" \
    --region="$REGION" \
    --project="$PROJECT_ID"

echo ">>> Updating Autohealing Policy..."
gcloud beta compute instance-groups managed update "$MIG_NAME" \
    --region="$REGION" \
    --health-check="https://www.googleapis.com/compute/v1/projects/${PROJECT_ID}/regions/${REGION}/healthChecks/lakefs-health-check" \
    --initial-delay=300 \
    --project="$PROJECT_ID"

rm runtime-startup.sh
echo ">>> Part 3B Complete."

# --- CONFIGURATION (Cont.) ---
echo ">>> Starting Part 4: Load Balancing & Ingress..."

if ! gcloud compute networks subnets describe "${PROXY_SUBNET_NAME}" --region="$REGION" --project="$PROJECT_ID" &>/dev/null; then
    gcloud compute networks subnets create "${PROXY_SUBNET_NAME}" \
        --purpose=REGIONAL_MANAGED_PROXY \
        --role=ACTIVE \
        --region="$REGION" \
        --network="$VPC_NAME" \
        --range="$LB_RANGE" \
        --project="$PROJECT_ID"
else
    echo ">>> Proxy subnet already exists."
fi

# 1. Internal Backend Service
if ! gcloud compute backend-services describe "${LB_PREFIX}-backend" --region="$REGION" --project="$PROJECT_ID" &>/dev/null; then
    gcloud compute backend-services create "${LB_PREFIX}-backend" \
        --load-balancing-scheme=INTERNAL_MANAGED \
        --protocol=HTTP \
        --port-name=http \
        --health-checks="lakefs-health-check" \
        --health-checks-region="$REGION" \
        --region="$REGION" \
        --project="$PROJECT_ID"
else
    echo ">>> Backend Service already exists."
fi

# Add Backend
BACKENDS=$(gcloud compute backend-services describe "${LB_PREFIX}-backend" --region="$REGION" --project="$PROJECT_ID" --format="value(backends)" || true)
if [[ -z "$BACKENDS" ]]; then
    gcloud compute backend-services add-backend "${LB_PREFIX}-backend" \
        --instance-group="$MIG_NAME" \
        --instance-group-region="$REGION" \
        --balancing-mode=UTILIZATION \
        --max-utilization=0.8 \
        --region="$REGION" \
        --project="$PROJECT_ID"
fi

# 2. URL Map
if ! gcloud compute url-maps describe "${LB_PREFIX}-url-map" --region="$REGION" --project="$PROJECT_ID" &>/dev/null; then
    gcloud compute url-maps create "${LB_PREFIX}-url-map" \
        --default-service="${LB_PREFIX}-backend" \
        --region="$REGION" \
        --project="$PROJECT_ID"
fi

# 3. Target Proxy
if ! gcloud compute target-http-proxies describe "${LB_PREFIX}-http-proxy" --region="$REGION" --project="$PROJECT_ID" &>/dev/null; then
    gcloud compute target-http-proxies create "${LB_PREFIX}-http-proxy" \
        --url-map="${LB_PREFIX}-url-map" \
        --region="$REGION" \
        --project="$PROJECT_ID"
fi

# 4. Firewall Rule (Updated for Legacy Health Check Ranges)
if ! gcloud compute firewall-rules describe allow-proxy-to-mig --project="$PROJECT_ID" &>/dev/null; then
    echo ">>> Creating Firewall Rule for LB & Health Checks..."
    gcloud compute firewall-rules create allow-proxy-to-mig \
        --network="$VPC_NAME" \
        --allow=tcp:8000 \
        --source-ranges="$LB_RANGE,35.191.0.0/16,130.211.0.0/22" \
        --target-tags="lakefs-server" \
        --description="Allow traffic from Proxy Subnet and Health Checks to LakeFS" \
        --project="$PROJECT_ID"
fi

# 5. Enable IAP SSH Access
if ! gcloud compute firewall-rules describe allow-ssh-ingress-from-iap --project="$PROJECT_ID" &>/dev/null; then
    echo ">>> Creating Firewall Rule for IAP SSH..."
    gcloud compute firewall-rules create allow-ssh-ingress-from-iap \
        --network="$VPC_NAME" \
        --direction=INGRESS \
        --action=ALLOW \
        --rules=tcp:22 \
        --source-ranges=35.235.240.0/20 \
        --target-tags="lakefs-server" \
        --project="$PROJECT_ID"
fi

# 6. Forwarding Rule
if ! gcloud compute forwarding-rules describe "${LB_PREFIX}-forwarding-rule" --region="$REGION" --project="$PROJECT_ID" &>/dev/null; then
    gcloud compute forwarding-rules create "${LB_PREFIX}-forwarding-rule" \
        --load-balancing-scheme=INTERNAL_MANAGED \
        --network="$VPC_NAME" \
        --subnet="$PROXY_SUBNET_NAME" \
        --ports=80 \
        --target-http-proxy="${LB_PREFIX}-http-proxy" \
        --target-http-proxy-region="$REGION" \
        --region="$REGION" \
        --project="$PROJECT_ID"
fi

LB_IP_ADDRESS=$(gcloud compute forwarding-rules describe "${LB_PREFIX}-forwarding-rule" --region="$REGION" --format="value(IPAddress)")
echo ">>> Internal LB Created at: $LB_IP_ADDRESS"

echo ">>> Part 4 Complete."

# --- CONFIGURATION (Cont.) ---
export GC_SA_NAME="lakefs-gc-sa"
export SCHEDULER_SA_NAME="lakefs-scheduler-sa"
export GC_JOB_NAME="lakefs-gc-daily"
export GC_SA_EMAIL="${GC_SA_NAME}@${PROJECT_ID}.iam.gserviceaccount.com"
export SCHEDULER_SA_EMAIL="${SCHEDULER_SA_NAME}@${PROJECT_ID}.iam.gserviceaccount.com"

echo ">>> Starting Part 5: Maintenance Automation..."

# Service Accounts
if ! gcloud iam service-accounts describe "$GC_SA_EMAIL" --project="$PROJECT_ID" &>/dev/null; then
    gcloud iam service-accounts create "$GC_SA_NAME" --display-name="LakeFS GC Runner" --project="$PROJECT_ID"
fi

if ! gcloud iam service-accounts describe "$SCHEDULER_SA_EMAIL" --project="$PROJECT_ID" &>/dev/null; then
    gcloud iam service-accounts create "$SCHEDULER_SA_NAME" --display-name="LakeFS Scheduler Trigger" --project="$PROJECT_ID"
fi

echo ">>> Waiting 20s for IAM propagation..."
sleep 20

# IAM Roles
gcloud storage buckets add-iam-policy-binding "gs://${BUCKET_NAME}" --member="serviceAccount:${GC_SA_EMAIL}" --role="roles/storage.objectAdmin" >/dev/null
gcloud projects add-iam-policy-binding "$PROJECT_ID" --member="serviceAccount:${GC_SA_EMAIL}" --role="roles/dataproc.worker" --condition=None >/dev/null
gcloud projects add-iam-policy-binding "$PROJECT_ID" --member="serviceAccount:${SCHEDULER_SA_EMAIL}" --role="roles/dataproc.editor" --condition=None >/dev/null
gcloud iam service-accounts add-iam-policy-binding "$GC_SA_EMAIL" --member="serviceAccount:${SCHEDULER_SA_EMAIL}" --role="roles/iam.serviceAccountUser" >/dev/null

echo ">>> Ensuring Dataproc service agent subnet access..."
PROJECT_NUMBER=$(gcloud projects describe "$PROJECT_ID" --format="value(projectNumber)")
DATAPROC_SA="service-${PROJECT_NUMBER}@dataproc-accounts.iam.gserviceaccount.com"
DATAPROC_SA_ALT="service-${PROJECT_NUMBER}@gcp-sa-dataproc.iam.gserviceaccount.com"

for DP_SA in "$DATAPROC_SA" "$DATAPROC_SA_ALT"; do
    if gcloud iam service-accounts describe "$DP_SA" --project="$PROJECT_ID" &>/dev/null; then
        gcloud compute networks subnets add-iam-policy-binding "$SUBNET_NAME" \
            --region="$REGION" \
            --member="serviceAccount:${DP_SA}" \
            --role="roles/compute.networkUser" \
            --project="$PROJECT_ID" >/dev/null
    fi
done

# --- AUTOMATED SMOKE TEST & SETUP ---
echo ">>> Starting Automated Setup & Verification..."

echo ">>> Waiting for MIG to be stable..."
# The fix to runtime-startup.sh should allow this to pass now
gcloud compute instance-groups managed wait-until --stable "$MIG_NAME" --region="$REGION" --project="$PROJECT_ID"

# 1. Fetch the full URL of a random instance
INSTANCE_URL=$(gcloud compute instance-groups managed list-instances "$MIG_NAME" \
    --region="$REGION" --project="$PROJECT_ID" --limit=1 --format="value(instance)")

if [[ -z "$INSTANCE_URL" ]]; then
    echo "ERROR: No instances found in MIG $MIG_NAME."
    exit 1
fi

# 2. Extract the Instance Name and Zone from the URL
INSTANCE_NAME=${INSTANCE_URL##*/}
INSTANCE_ZONE=${INSTANCE_URL#*zones/}
INSTANCE_ZONE=${INSTANCE_ZONE%%/*}

if [[ -z "$INSTANCE_ZONE" ]]; then
    echo "ERROR: Failed to determine instance zone from $INSTANCE_URL"
    exit 1
fi

echo ">>> Admin Access Key: $ACCESS_KEY_ID"
echo ">>> Fetch secret access key with:"
echo "    gcloud secrets versions access latest --secret=lakefs-secret-access-key --project=$PROJECT_ID"

# 2. Create Example Repository (Idempotent)
echo ">>> Ensuring 'example-repo' exists..."
# We SSH again to run the curl command using the newly acquired credentials
# Note: Basic Auth uses AccessKey:SecretKey
gcloud compute ssh "$INSTANCE_NAME" --zone="$INSTANCE_ZONE" --tunnel-through-iap --project="$PROJECT_ID" --quiet \
    --command "curl -s -X POST http://localhost:8000/api/v1/repositories \
    -u \"$ACCESS_KEY_ID:$SECRET_ACCESS_KEY\" \
    -H 'Content-Type: application/json' \
    -d '{\"name\":\"example-repo\",\"storage_namespace\":\"gs://${BUCKET_NAME}/example-repo\",\"default_branch\":\"main\"}'" \
    -- -o StrictHostKeyChecking=no >/dev/null 2>&1 || echo "    (Repo creation skipped or failed, possibly already exists)"

# 3. Generate Batch JSON with REAL Credentials
cat <<EOF > batch-request.json
{
  "sparkBatch": {
    "mainClass": "io.treeverse.gc.GarbageCollectorDriver",
    "args": ["run", "--repo", "example-repo", "--region", "${REGION}", "--mark", "gs://${BUCKET_NAME}/_lakefs/gc/mark", "--sweep", "gs://${BUCKET_NAME}/_lakefs/gc/sweep"],
    "runtimeConfig": {
        "containerImage": "treeverse/lakefs-spark-gc:latest",
        "properties": {
            "spark.hadoop.lakefs.api.url": "http://${LB_IP_ADDRESS}/api/v1",
            "spark.hadoop.lakefs.api.access_key": "${ACCESS_KEY_ID}",
            "spark.hadoop.lakefs.api.secret_key": "${SECRET_ACCESS_KEY}"
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

# Cloud Scheduler
if ! gcloud scheduler jobs describe "$GC_JOB_NAME" --location="$REGION" --project="$PROJECT_ID" &>/dev/null; then
    gcloud scheduler jobs create http "$GC_JOB_NAME" \
        --location="$REGION" \
        --schedule="0 3 * * 3" \
        --time-zone="Etc/UTC" \
        --uri="https://dataproc.googleapis.com/v1/projects/${PROJECT_ID}/locations/${REGION}/batches" \
        --http-method=POST \
        --oauth-service-account-email="$SCHEDULER_SA_EMAIL" \
        --headers="Content-Type=application/json" \
        --message-body-from-file=batch-request.json \
        --project="$PROJECT_ID"
else
    echo ">>> Updating existing Scheduler Job with valid credentials..."
    gcloud scheduler jobs update http "$GC_JOB_NAME" \
        --location="$REGION" \
        --schedule="0 3 * * 3" \
        --oauth-service-account-email="$SCHEDULER_SA_EMAIL" \
        --message-body-from-file=batch-request.json \
        --project="$PROJECT_ID"
fi

rm batch-request.json

echo "----------------------------------------------------"
echo ">>> COMPLETE. LakeFS Infrastructure is deployed & configured."
echo "----------------------------------------------------"
echo ">>> Connect to your instance via IAP Tunnel:"
echo "    gcloud compute ssh $INSTANCE_NAME --zone=${INSTANCE_ZONE} --tunnel-through-iap -- -L 8080:${LB_IP_ADDRESS}:80"
echo ">>> Then visit: http://localhost:8080"
echo "----------------------------------------------------"
