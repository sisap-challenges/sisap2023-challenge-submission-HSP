docker build --no-cache -t sisap23/hsp .
docker run -v /home/sisap23evaluation/data:/data:ro -v ./result:/result -it sisap23/hsp 300K
